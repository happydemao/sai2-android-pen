package com.winlator.sai;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;

/**
 * PaintTool SAI Ver.2 压感桥 —— 安卓侧发送端。
 *
 * <h3>它对接的是什么</h3>
 * macOS 项目 ametrien/Paint-Tool-SAI-pen-pressure-macOS-fix（MIT）用一个替换版
 * {@code wintab32.dll} 给 SAI 供笔压。该 DLL <b>只认一份运行在 127.0.0.1:47800 上的
 * UDP 文本报文</b>，与操作系统无关；它自己是 Win64 的 DLL，因此在
 * Box64 + Wine 里能原样运行。
 *
 * 于是"移植到安卓"只剩下一个问题：<b>谁来发这份报文</b>。
 * macOS 的答案是一个读 CGEventTap 的原生助手；安卓没有全局触摸监听
 * （只有前台 App 拿得到 MotionEvent），而在 Winlator 里前台 App 就是 Winlator 本身，
 * 所以发送端只能长在 Winlator 的输入管线里 —— 也就是本类。
 *
 * <h3>报文格式（来自 wintab32.dll 的 wtc_parse_datagram，勿改）</h3>
 * <pre>
 *   笔样本: "seq p x y w h"                 （可再多三字段 ev tx n，可省略）
 *   缩放:   "Z &lt;steps&gt;"                     （steps 非 0，|steps| ≤ 64）
 *   唤醒:   "K &lt;token&gt;"
 * </pre>
 * <ul>
 *   <li>{@code seq}  递增序号（只用于对端统计丢包，可任意递增）</li>
 *   <li>{@code p}    压力，0..pmax；0 = 抬笔/悬停</li>
 *   <li>{@code x,y}  位置，<b>原点在左下、y 向上</b>，与 {@code w,h} 同单位</li>
 *   <li>{@code w,h}  参照系尺寸；DLL 只按 {@code x/w}、{@code (h-y)/h} 的比例换算，
 *                    所以直接填容器分辨率（Windows 像素）即可</li>
 * </ul>
 *
 * <p>报文必须是 ASCII、无换行、单包一帧。</p>
 *
 * <h3>使用前置条件</h3>
 * <ol>
 *   <li>DLL 放到容器的 {@code C:\windows\system32\wintab32.dll}</li>
 *   <li>容器环境变量 {@code WINEDLLOVERRIDES=wintab32=n,b}</li>
 *   <li>{@code C:\wt_pmax.txt} 写上限（如 {@code 4095}），DLL 启动时只读一次</li>
 *   <li>SAI 里打开 "Use WinTab API"（选项 → 数位板/WinTab）</li>
 * </ol>
 *
 * <p>非线程安全问题是不可接受的：所有方法都可能从 UI 线程调用，
 * 内部用一把锁 + 一个常驻发送线程，绝不阻塞 onTouchEvent。</p>
 */
public final class SaiPressureBridge {

    /** DLL 的监听端口，写死在 wintab32.c 的 SAMPLE_PORT。 */
    public static final int PORT = 47800;

    /** 压力上限。必须与容器里 C:\wt_pmax.txt 的值一致，否则一端被截断。 */
    private static final int PMAX = 4095;

    private static volatile boolean enabled = false;
    private static DatagramSocket socket;
    private static InetAddress loopback;
    private static int seq = 1;

    /** 容器分辨率（Windows 像素）。由 {@link #configure} 在启动时喂进来。 */
    private static volatile int screenW = 1600;
    private static volatile int screenH = 900;

    /** 最新的样本，由发送线程取走，避免在 UI 线程做 syscall。 */
    private static final Object LOCK = new Object();
    private static int pendingP = -1, pendingX, pendingY;
    private static boolean pendingZoom = false;
    private static int pendingZoomSteps = 0;

    private static Thread pump;

    private SaiPressureBridge() {}

    /**
     * 启用前调用一次。容器分辨率必须是 Winlator 的 Screen size，不是手机物理分辨率。
     *
     * @return 是否成功（失败通常是 socket 创建异常，可安全忽略并退化为无压感）
     */
    public static synchronized boolean configure(int containerWidth, int containerHeight) {
        if (containerWidth > 0) screenW = containerWidth;
        if (containerHeight > 0) screenH = containerHeight;
        if (enabled) return true;
        try {
            loopback = InetAddress.getByName("127.0.0.1");
            socket = new DatagramSocket();       // 随机本地端口，够用
            socket.setTrafficClass(0);
            enabled = true;
            pump = new Thread(SaiPressureBridge::pumpLoop, "SAI-pressure-bridge");
            pump.setDaemon(true);
            pump.setPriority(Thread.MAX_PRIORITY);
            pump.start();
            return true;
        } catch (Exception e) {
            enabled = false;
            return false;
        }
    }

    public static void shutdown() {
        enabled = false;
        if (pump != null) { pump.interrupt(); pump = null; }
        if (socket != null) { socket.close(); socket = null; }
    }

    public static boolean isEnabled() {
        return enabled;
    }

    /** 容器分辨率变了（改了 Screen size 后重启前调用）。 */
    public static void setScreenSize(int w, int h) {
        if (w > 0 && h > 0) { screenW = w; screenH = h; }
    }

    // ------------------------------------------------------------------
    // 输入侧 API
    // ------------------------------------------------------------------

    /**
     * 一个笔样本。位置用 X 服务器坐标（左上原点、y 向下），本方法内部翻成
     * WinTab 的左下原点 y 向上。
     *
     * @param xPx        笔尖 x（容器像素）
     * @param yPx        笔尖 y（容器像素，y 向下）
     * @param pressure01 归一化压力 0..1（{@code MotionEvent.getPressure()}）
     * @param penDown    true = 笔尖接触，false = 悬停/抬起
     */
    public static void onPenSample(float xPx, float yPx, float pressure01, boolean penDown) {
        if (!enabled) return;
        int p = penDown ? Math.round(clamp(pressure01, 0f, 1f) * PMAX) : 0;
        if (penDown && p < 1) p = 1;          // 接触了就别报 0，否则被当成抬笔

        // y 翻向：DLL 用的是 (h - y)，这里直接给 y-up，让它算出屏幕 y。
        int x = Math.round(xPx);
        int y = Math.round(screenH - yPx);

        synchronized (LOCK) {
            pendingP = p;
            pendingX = x;
            pendingY = y;
            LOCK.notifyAll();
        }
    }

    /** 抬笔：发一个压力 0 的悬停样本，让 SAI 收笔。 */
    public static void onPenUp(float xPx, float yPx) {
        onPenSample(xPx, yPx, 0f, false);
    }

    /**
     * 双指缩放 → SAI 缩放。
     *
     * <p>DLL 收到后在 SAI 进程里直接 PostMessage(WM_MOUSEWHEEL)，所以不需要
     * Wine 层的键鼠模拟，也不需要把 Ctrl 按下去。</p>
     *
     * @param steps 非 0；正负决定方向，绝对值 ≤ 64（DLL 会丢弃越界值）
     */
    public static void zoom(int steps) {
        if (!enabled || steps == 0) return;
        int s = Math.max(-64, Math.min(64, steps));
        synchronized (LOCK) {
            pendingZoomSteps = s;
            pendingZoom = true;
            LOCK.notifyAll();
        }
    }

    // ------------------------------------------------------------------
    // 发送线程：合并样本 + 限速，绝不阻塞 UI
    // ------------------------------------------------------------------

    private static void pumpLoop() {
        final byte[] buf = new byte[96];
        while (enabled) {
            int p, x, y, w, h;
            boolean doZoom;
            int zoomSteps;

            // 取快照并清空（同一把锁内，不会丢样本：新样本只会覆盖 pending）
            synchronized (LOCK) {
                if (pendingP < 0 && !pendingZoom) {
                    try {
                        LOCK.wait(50);            // idle 时不吃 CPU
                    } catch (InterruptedException e) {
                        return;
                    }
                }
                p = pendingP; x = pendingX; y = pendingY;
                w = screenW; h = screenH;
                pendingP = -1;
                doZoom = pendingZoom; zoomSteps = pendingZoomSteps; pendingZoom = false;
            }

            if (p >= 0) {
                int n = formatSample(buf, seq++, p, x, y, w, h);
                send(buf, n);
            }
            if (doZoom) {
                byte[] z = ("Z " + zoomSteps + "\n").getBytes(StandardCharsets.US_ASCII);
                send(z, z.length);
            }
        }
    }

    /** "seq p x y w h" —— 字段顺序是与 DLL 的硬约定，改了就全乱。 */
    private static int formatSample(byte[] out, int seq, int p, int x, int y, int w, int h) {
        String s = seq + " " + p + " " + x + " " + y + " " + w + " " + h;
        byte[] b = s.getBytes(StandardCharsets.US_ASCII);
        int n = Math.min(b.length, out.length);
        System.arraycopy(b, 0, out, 0, n);
        return n;
    }

    private static void send(byte[] data, int len) {
        DatagramSocket s = socket;
        if (s == null || s.isClosed()) return;
        try {
            s.send(new DatagramPacket(data, len, loopback, PORT));
        } catch (IOException ignored) {
            // 对端不在（SAI 还没起，或 DLL 没加载）：丢掉即可，这是流不是队列
        }
    }

    private static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
}
