package com.winlator.sai;

import android.os.SystemClock;
import android.util.Log;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;

/**
 * 内置在 Winlator 里的数位板桥（无 root、无命令、无常驻进程）。
 *
 * smali 侧只有两处调用：
 *   - {@link #configure(int, int)}        容器分辨率（updateXform 里）
 *   - {@link #handle(View, MotionEvent)}  TouchpadView 的触摸/悬停入口
 *
 * handle() 做三件事，全部在 Java 里完成（上一版把分支写在 smali 里导致
 * VerifyError，一进容器就 FATAL）：
 *   1. 采样压感 → UDP:47800 → wintab32.dll → SAI
 *   2. 识别数位板（设备名含 "Pen"；实测 getToolType()/getSource() 都不可靠）
 *   3. 用压力合成 X 指针左键按下/抬起 —— 根治"笔能画、点不了 UI"：
 *      这支 BLE 笔只送 HOVER_MOVE/ACTION_MOVE + 压力，从不送 ACTION_BUTTON_PRESS，
 *      所以 Winlator 原生逻辑永远不会按下左键。
 *
 * 这里不直接引用 com.winlator.* 的类，全部走反射：
 *  - 编译只需要 android.jar，不用维护 stub
 *  - 任何反射失败都只是"笔点击失效"，不会拖垮 App
 *
 * 报文： "seq pressure x y w h"（x,y 原点左下、y 向上；w,h 为容器分辨率）
 */
public final class PenBridge {

    private static final String TAG = "SaiPen";
    private static final int PORT = 47800;
    private static final int PMAX = 4095;

    /**
     * 落笔判据分两层：
     *  - 动作层：ACTION_DOWN/ACTION_MOVE 只会在笔尖真正接触时出现（悬停是 HOVER_*）
     *  - 压力层：兜底，笔若只靠压力表达接触（某些驱动），压力够大也算按下
     * 只用压力会被悬停抖动（0.09 上下）骗出误点击，所以动作优先。
     */
    private static final float PRESS_MIN = 0.02f;
    private static final float PRESS_STRONG = 0.35f;
    /** 超过这么久没收到笔事件就认为笔已离开（BLE 可能不给 HOVER_EXIT）。 */
    private static final long LOST_MS = 700;

    private static final Object LOCK = new Object();

    private static DatagramSocket socket;
    private static InetAddress loopback;
    private static boolean starting;
    private static boolean disabled;

    private static int screenW = 1280;
    private static int screenH = 720;

    private static int pendingX, pendingY, pendingP;
    private static boolean hasPending;

    private static int seq = 1;
    private static int sentCount;
    private static int lastAction = -1;
    private static float lastPressure = -1f;

    /* ---- 反射句柄（一次性解析并缓存） ---- */
    private static boolean reflectTried;
    private static Field fXServer;
    private static Field fXform;
    private static Method mTransformPoint;
    private static Method mInjectPointerMove;
    private static Method mInjectPress;
    private static Method mInjectRelease;
    private static Object BUTTON_LEFT;

    /** 我们合成出来的左键是否处于按下状态。 */
    private static boolean penDown;
    private static long lastPenEventMs;
    /** 只在第一次见到笔时打一条诊断日志（设备源类型）。 */
    private static boolean loggedPen;

    private PenBridge() {}

    /** updateXform 调用，告知当前容器的显示分辨率（Windows 像素）。 */
    public static void configure(int width, int height) {
        try {
            if (width > 0 && height > 0) {
                screenW = width;
                screenH = height;
            }
        } catch (Throwable ignored) {
        }
    }

    /** 诊断日志。 */
    public static void note(String s) {
        try {
            Log.d(TAG, s);
        } catch (Throwable ignored) {
        }
    }

    /**
     * 统一入口。smali：
     *   invoke-static {p0, p1}, Lcom/winlator/sai/PenBridge;->handle(Landroid/view/View;Landroid/view/MotionEvent;)V
     * 任何异常都吞掉：桥坏了只应退化，绝不能让 Winlator 崩。
     */
    public static void handle(View pad, MotionEvent ev) {
        if (pad == null || ev == null) return;
        try {
            final int action = ev.getActionMasked();
            final float pressure = ev.getPressure();

            // 诊断：设备没有 logcat 时写到 app 自己的目录，adb root 可直接读。
            // 悬停事件太多，只记非悬停（DOWN/MOVE/UP 等）。
            if (action != MotionEvent.ACTION_HOVER_MOVE) {
                flog("ev act=" + action
                        + " src=0x" + Integer.toHexString(ev.getSource())
                        + " tool=" + ev.getToolType(0)
                        + " pr=" + pressure
                        + " synth=" + needsSyntheticClick(ev)
                        + " t=" + SystemClock.uptimeMillis());
            }

            int[] xy = toContainer(pad, ev);
            if (xy == null) return;
            final int x = xy[0], y = xy[1];

            // 1) 压感采样（手指与笔都送）
            emit(x, y, pressure, action);

            // 2) 只有"鼠标型笔"才合成左键（正规 STYLUS 交给 Winlator 原生处理）
            if (isPen(ev)) {
                if (!loggedPen) {
                    loggedPen = true;
                    note("pen src=0x" + Integer.toHexString(ev.getSource())
                            + " tool=" + ev.getToolType(0)
                            + " synth=" + needsSyntheticClick(ev));
                }
                if (needsSyntheticClick(ev)) {
                    syncLeftButton(pad, x, y, pressure, action);
                } else {
                    // 正规 STYLUS（S Pen）：原生 tap 已经会点，我们只在拖动/长按时补按下
                    syncDragOnly(pad, x, y, pressure, ev, action);
                }
            }
        } catch (Throwable ignored) {
        }
    }

    /** 诊断日志写文件（这台设备 logd 被关，logcat 读不到任何东西）。 */
    private static java.io.FileWriter logWriter;
    private static int logCount;

    private static void flog(String s) {
        if (logCount > 400) return;
        try {
            if (logWriter == null) {
                logWriter = new java.io.FileWriter("/data/data/com.winlator/files/pb.log", false);
            }
            logWriter.write(s + "\n");
            logWriter.flush();
            logCount++;
        } catch (Throwable ignored) {
        }
    }

    private static boolean isPen(MotionEvent ev) {
        try {
            InputDevice dev = ev.getDevice();
            if (dev != null) {
                String name = dev.getName();
                if (name != null && name.toLowerCase().contains("pen")) return true;
            }
            return ev.getToolType(0) == MotionEvent.TOOL_TYPE_STYLUS;
        } catch (Throwable t) {
            return false;
        }
    }

    /**
     * 只有"鼠标型笔"需要我们自己合成左键。
     *
     * Winlator 的 TouchpadView.onTouchEvent 在 ACTION_DOWN 分支第一行就对 SOURCE_MOUSE
     * 直接 return true，所以像 WH851-545 那支 BLE 笔（SOURCE_MOUSE）永远不会按下左键，
     * 必须由我们补上。
     *
     * 而 Galaxy Tab 的 S Pen 是正规 STYLUS（Sources = TOUCHSCREEN|STYLUS，没有 MOUSE），
     * 原生触摸分支本来就会按下/抬起；我们再合成一次，就变成"一次点击 = 两次 click"，
     * 表现就是点一下被当成双击。
     */
    private static boolean needsSyntheticClick(MotionEvent ev) {
        try {
            // 只认 SOURCE_MOUSE 的 0x2000 位。
            // 不能写 (src & SOURCE_MOUSE) != 0：SOURCE_MOUSE=0x2002 里含 SOURCE_CLASS_POINTER(0x0002)，
            // 而 S Pen 的 0x5002 = STYLUS|TOUCHSCREEN|CLASS_POINTER 也带这一位，会误判成鼠标型笔
            // → 原生已经按过一次，我们再按一次 → 一次点击变成两声。
            return (ev.getSource() & 0x2000) != 0;
        } catch (Throwable t) {
            return false;
        }
    }

    private static int[] toContainer(View pad, MotionEvent ev) {
        resolveReflection();
        if (fXform == null || mTransformPoint == null) return null;
        try {
            float[] xform = (float[]) fXform.get(pad);
            if (xform == null) return null;
            float[] p = (float[]) mTransformPoint.invoke(null,
                    xform, ev.getX(), ev.getY());
            if (p == null || p.length < 2) return null;
            return new int[]{ (int) p[0], (int) p[1] };
        } catch (Throwable t) {
            return null;
        }
    }

    private static void syncLeftButton(View pad, int x, int y, float pressure, int action) {
        resolveReflection();
        if (fXServer == null || mInjectPointerMove == null
                || mInjectPress == null || mInjectRelease == null || BUTTON_LEFT == null) {
            return;
        }
        try {
            Object xs = fXServer.get(pad);
            if (xs == null) return;

            final long now = SystemClock.uptimeMillis();
            final long gap = now - lastPenEventMs;
            lastPenEventMs = now;

            // 笔离开感应区时 BLE 未必给 HOVER_EXIT，超时就自己松开，避免左键"粘住"
            if (penDown && gap > LOST_MS) {
                penDown = false;
                mInjectRelease.invoke(xs, BUTTON_LEFT);
                note("PEN_UP lost gap=" + gap);
            }

            final boolean lifted = action == MotionEvent.ACTION_UP
                    || action == MotionEvent.ACTION_CANCEL
                    || action == MotionEvent.ACTION_HOVER_EXIT
                    || action == MotionEvent.ACTION_POINTER_UP;
            final boolean touching = action == MotionEvent.ACTION_DOWN
                    || action == MotionEvent.ACTION_MOVE
                    || action == MotionEvent.ACTION_POINTER_DOWN;

            if (!penDown) {
                // 触摸动作出现即代表笔尖接触（悬停走 HOVER_*，压力恒 0）；
                // 这支笔接触压力只有 0.03、移动时甚至 0.005，所以不能把压力当主判据。
                if (!lifted && (touching || pressure > PRESS_STRONG)) {
                    penDown = true;
                    // 先把光标移到笔尖再按下，顺序反了会点在旧位置
                    mInjectPointerMove.invoke(xs, x, y);
                    mInjectPress.invoke(xs, BUTTON_LEFT);
                    note("PEN_DOWN " + x + "," + y + " pr=" + pressure + " act=" + action);
                    flog("SYNTH DOWN pr=" + pressure + " act=" + action);
                }
            } else if (lifted || (!touching && pressure < PRESS_MIN)) {
                penDown = false;
                mInjectRelease.invoke(xs, BUTTON_LEFT);
                note("PEN_UP pr=" + pressure + " act=" + action);
                flog("SYNTH UP pr=" + pressure + " act=" + action);
            } else {
                // 画线/拖动期间光标持续跟随笔尖
                mInjectPointerMove.invoke(xs, x, y);
            }
        } catch (Throwable t) {
            penDown = false;
            try { Log.w(TAG, "click failed: " + t); } catch (Throwable ignored) { }
        }
    }

    /**
     * 正规 STYLUS（Galaxy Tab 的 S Pen 这类，source = 0x5002 没有 0x2000 位）。
     *
     * 原生 TouchpadView 的行为：
     *   - 单指 MOVE 只 injectPointerMove（移动光标），**从不按左键** → 没有"拖动"
     *   - 抬起时 isTap()（时长<200ms 且 位移<10px）才 press+release 一次 → 点击
     *
     * 所以我们只做"拖动/长按"这半边，和 isTap() 严格互补，避免一次点击变两声：
     *   - 位移 ≥10px（屏幕坐标，与原生 travelDistance 同口径）或 按住 ≥200ms → 我们按下并保持
     *   - 抬手时如果是我们按下的，才松开
     *   - 快速轻点：我们什么都不做，交给原生点击
     */
    private static final float DRAG_PX = 10f;
    private static final long LONG_PRESS_MS = 200;

    private static float downSX, downSY;
    private static long downEventMs;
    private static boolean dragPressed;

    private static void syncDragOnly(View pad, int x, int y, float pressure,
                                     MotionEvent ev, int action) {
        resolveReflection();
        if (fXServer == null || mInjectPointerMove == null
                || mInjectPress == null || mInjectRelease == null || BUTTON_LEFT == null) {
            return;
        }
        try {
            Object xs = fXServer.get(pad);
            if (xs == null) return;

            final long now = SystemClock.uptimeMillis();
            // 笔离开感应区却没有 UP：自己松开，避免左键粘死
            if (dragPressed && now - lastPenEventMs > LOST_MS) {
                dragPressed = false;
                mInjectRelease.invoke(xs, BUTTON_LEFT);
                flog("DRAG_UP lost");
            }
            lastPenEventMs = now;

            switch (action) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_POINTER_DOWN:
                    downSX = ev.getX();
                    downSY = ev.getY();
                    downEventMs = ev.getEventTime();
                    dragPressed = false;
                    break;

                case MotionEvent.ACTION_MOVE: {
                    if (!dragPressed) {
                        final float dx = ev.getX() - downSX;
                        final float dy = ev.getY() - downSY;
                        final boolean moved = (dx * dx + dy * dy) >= DRAG_PX * DRAG_PX;
                        final boolean slow = (ev.getEventTime() - downEventMs) >= LONG_PRESS_MS;
                        if (moved || slow) {
                            dragPressed = true;
                            mInjectPointerMove.invoke(xs, x, y);
                            mInjectPress.invoke(xs, BUTTON_LEFT);
                            note("DRAG_DOWN " + x + "," + y);
                            flog("DRAG_DOWN pr=" + pressure + " moved=" + moved + " slow=" + slow);
                        }
                    } else {
                        mInjectPointerMove.invoke(xs, x, y);
                    }
                    break;
                }

                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                case MotionEvent.ACTION_POINTER_UP:
                    if (dragPressed) {
                        dragPressed = false;
                        mInjectRelease.invoke(xs, BUTTON_LEFT);
                        note("DRAG_UP");
                        flog("DRAG_UP pr=" + pressure + " act=" + action);
                    }
                    break;

                default:
                    break;
            }
        } catch (Throwable t) {
            dragPressed = false;
        }
    }

    private static void resolveReflection() {
        if (reflectTried) return;
        reflectTried = true;
        try {
            Class<?> viewCls = Class.forName("com.winlator.widget.TouchpadView");
            try {
                fXServer = viewCls.getDeclaredField("xServer");
                fXServer.setAccessible(true);
            } catch (Throwable ignored) { }
            try {
                fXform = viewCls.getDeclaredField("xform");
                fXform.setAccessible(true);
            } catch (Throwable ignored) { }

            Class<?> xformCls = Class.forName("com.winlator.math.XForm");
            mTransformPoint = xformCls.getMethod("transformPoint",
                    float[].class, float.class, float.class);

            Class<?> xsCls = Class.forName("com.winlator.xserver.XServer");
            mInjectPointerMove = xsCls.getMethod("injectPointerMove", int.class, int.class);

            Class<?> btnCls = Class.forName("com.winlator.xserver.Pointer$Button");
            BUTTON_LEFT = btnCls.getField("BUTTON_LEFT").get(null);
            mInjectPress = xsCls.getMethod("injectPointerButtonPress", btnCls);
            mInjectRelease = xsCls.getMethod("injectPointerButtonRelease", btnCls);

            Log.d(TAG, "reflect ok xServer=" + (fXServer != null)
                    + " xform=" + (fXform != null)
                    + " move=" + (mInjectPointerMove != null));
        } catch (Throwable t) {
            try { Log.w(TAG, "reflect failed: " + t); } catch (Throwable ignored) { }
        }
    }

    // ---------------------------------------------------------------- UDP 发送

    private static void emit(int x, int y, float pressure, int action) {
        try {
            if (disabled) return;

            lastAction = action;
            lastPressure = pressure;

            final boolean lifting = (action == 1 || action == 3 || action == 6 || action == 10);
            final boolean pressing = (action == 0 || action == 5);

            int p;
            if (lifting) {
                p = 0;
            } else {
                float pr = pressure;
                if (pr < 0f) pr = 0f;
                if (pr > 1f) pr = 1f;
                p = Math.round(pr * PMAX);
                if (pressing && p < 1) p = 1;   // 明确按下时至少给 1
                if (p < 1) p = 0;               // 悬停：只移动不落笔
            }

            synchronized (LOCK) {
                pendingX = x;
                pendingY = y;
                pendingP = p;
                hasPending = true;
                LOCK.notifyAll();
            }

            ensureWorker();
        } catch (Throwable t) {
            disabled = true;
            try { Log.w(TAG, "disabled: " + t); } catch (Throwable ignored) { }
        }
    }

    private static void ensureWorker() {
        synchronized (LOCK) {
            if (starting || socket != null) return;
            starting = true;
        }
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                workerLoop();
            }
        }, "SaiPenSender");
        t.setDaemon(true);
        t.setPriority(Thread.MAX_PRIORITY);
        t.start();
    }

    private static void workerLoop() {
        try {
            loopback = InetAddress.getByName("127.0.0.1");
            DatagramSocket s = new DatagramSocket();
            synchronized (LOCK) {
                socket = s;
                LOCK.notifyAll();
            }
            Log.i(TAG, "pen bridge up, pmax=" + PMAX);
        } catch (Throwable t) {
            disabled = true;
            try { Log.w(TAG, "init failed: " + t); } catch (Throwable ignored) { }
            return;
        }

        while (!disabled) {
            int x, y, p;
            synchronized (LOCK) {
                if (!hasPending) {
                    try {
                        LOCK.wait(200);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
                if (!hasPending) continue;
                x = pendingX; y = pendingY; p = pendingP;
                hasPending = false;
            }
            send(x, y, p);
        }
    }

    private static void send(int x, int y, int p) {
        if (x < 0) x = 0; else if (x >= screenW) x = screenW - 1;
        if (y < 0) y = 0; else if (y >= screenH) y = screenH - 1;

        int wireY = screenH - y;      // 报文 y 从底部量

        String msg = seq + " " + p + " " + x + " " + wireY + " " + screenW + " " + screenH;
        byte[] data = msg.getBytes(StandardCharsets.US_ASCII);
        try {
            socket.send(new DatagramPacket(data, data.length, loopback, PORT));
            seq++;
            sentCount++;
            if (sentCount <= 32 || (sentCount & 31) == 0) {
                Log.d(TAG, "sent=" + sentCount + " p=" + p + " xy=" + x + "," + y
                        + " act=" + lastAction + " pr=" + lastPressure);
            }
        } catch (Throwable t) {
            disabled = true;
            try { Log.w(TAG, "send failed: " + t); } catch (Throwable ignored) { }
        }
    }
}
