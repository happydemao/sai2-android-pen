package com.winlator.sai;

import android.util.Log;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;

/**
 * Android 侧的液化服务。
 *
 * 为什么是 app 而不是 Wine 里的 DLL：真正的像素处理应该跑在 ARM 原生代码上，
 * 而不是 Box64 模拟的 x86。DLL 只负责「拿剪贴板 / 放剪贴板 / 发 Ctrl+V」，
 * 中间 10 MB 的变形计算通过 127.0.0.1 交给这里（同一个 app 进程，localhost 可用）。
 *
 * 协议（大端）：
 *   request : magic 'LQ01', int w, h, mode, steps,
 *             float x0, y0, x1, y1, radius, strength, int len, byte[len] BGRA
 *   response: int status (0 ok), int len, byte[len]
 *
 * start() 是幂等的，由 TouchpadView.updateXform 里的 smali 钩子调用一次。
 */
public final class LiquifyServer {
    private static final String TAG = "LiquifyServer";
    public static final int PORT = 47801;
    private static final int MAGIC = 0x4C513031;   // "LQ01"

    private static volatile boolean started;

    private LiquifyServer() { }

    public static synchronized void start() {
        if (started) return;
        started = true;
        Thread t = new Thread(new Runnable() {
            @Override public void run() {
                prewarm();
                serve();
            }
        }, "liquify-server");
        t.setDaemon(true);
        t.start();
        Log.i(TAG, "listening on 127.0.0.1:" + PORT);
        /* logcat is not readable on the test device, so publish the two facts
         * that matter (native lib loaded? prewarm time) as a small file. */
        try {
            java.io.File f = new java.io.File("/data/data/lator.forsai/files/liquify_status.txt");
            java.io.FileOutputStream fo = new java.io.FileOutputStream(f);
            try {
                String txt = "native=" + (LiquifyWarp.nativeAvailable() ? 1 : 0) + "\nerr=" + LiquifyWarp.nativeError() + "\n";
                fo.write(txt.getBytes("UTF-8"));
            } finally { fo.close(); }
        } catch (Throwable ignored) { }
    }

    /** Load classes and let ART/JIT settle on a tiny canvas, so the first real
     *  stroke does not pay the ~3 s class-verification + compile cost. */
    private static void prewarm() {
        try {
            int n = 64;
            byte[] small = new byte[n * n * 4];
            for (int i = 0; i < small.length; i += 4) {
                small[i] = (byte) 0xFF; small[i + 1] = (byte) 0xFF;
                small[i + 2] = (byte) 0xFF; small[i + 3] = (byte) 0xFF;
            }
            /* ART's JIT compiles a method only after enough invocations; three
             * calls left the first real 1600x1600 warp interpreted (~3 s).
             * 12000 tiny calls cost well under a second and make the method hot
             * before the user ever runs the tool. */
            for (int k = 0; k < 12000; ++k) {
                LiquifyWarp.warp(small, n, n, LiquifyWarp.FORWARD,
                        20, 32, 44, 32, 16, 0.8f, 4);
            }
            Log.i(TAG, "prewarmed");
        } catch (Throwable t) {
            Log.w(TAG, "prewarm failed: " + t);
        }
    }

    private static void serve() {
        ServerSocket ss = null;
        try {
            ss = new ServerSocket(PORT, 4, InetAddress.getByName("127.0.0.1"));
        } catch (IOException e) {
            Log.w(TAG, "bind failed: " + e);
            started = false;
            return;
        }
        while (true) {
            Socket s = null;
            try {
                s = ss.accept();
                s.setTcpNoDelay(true);
                handle(s);
            } catch (IOException e) {
                Log.w(TAG, "conn: " + e);
            } finally {
                if (s != null) try { s.close(); } catch (IOException ignored) { }
            }
        }
    }

    private static void handle(Socket s) throws IOException {
        DataInputStream in = new DataInputStream(s.getInputStream());
        DataOutputStream out = new DataOutputStream(s.getOutputStream());
        int magic = in.readInt();
        if (magic != MAGIC) {
            out.writeInt(2); out.writeInt(0); out.flush();
            return;
        }
        int w = in.readInt();
        int h = in.readInt();
        int mode = in.readInt();
        int steps = in.readInt();
        float x0 = in.readFloat(), y0 = in.readFloat();
        float x1 = in.readFloat(), y1 = in.readFloat();
        float radius = in.readFloat(), strength = in.readFloat();
        int len = in.readInt();
        if (w <= 0 || h <= 0 || len != w * h * 4 || len > (64 << 20)) {
            out.writeInt(3); out.writeInt(0); out.flush();
            return;
        }
        byte[] img = new byte[len];
        in.readFully(img);

        long t0 = System.currentTimeMillis();
        byte[] res;
        try {
            res = LiquifyWarp.warp(img, w, h, mode, x0, y0, x1, y1, radius, strength, steps);
        } catch (Throwable t) {
            Log.w(TAG, "warp failed: " + t);
            out.writeInt(4); out.writeInt(0); out.flush();
            return;
        }
        long dt = System.currentTimeMillis() - t0;

        out.writeInt(0);
        out.writeInt(res.length);
        out.write(res);
        out.writeInt((int) dt);   /* warp time, so the DLL can log it */
        out.flush();
        Log.i(TAG, "warp " + w + "x" + h + " mode=" + mode + " steps=" + steps
                + " in " + dt + " ms");
    }
}
