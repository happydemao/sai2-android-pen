import java.io.FileInputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;

/**
 * 数位板 → SAI 压感桥的发送端（Android 侧）。
 *
 * 直接读 /dev/input/eventX 的原始 evdev 事件，按 wintab32.dll 的 UDP 文本报文
 * 转发给容器里正在跑的 SAI。不需要编 APK、不需要改 Winlator、不需要 Python。
 *
 * 运行方式（需 root）：
 *   CLASSPATH=/data/local/tmp/pensender.dex app_process /system/bin PenSender \
 *        /dev/input/event8 1280 720 4095 250
 * 参数：设备 容器宽 容器高 pmax 最大发送频率(Hz)
 *
 * 报文格式（与 wintab32.dll 的 wtc_parse_datagram 对齐）：
 *   "seq pressure x y w h"   x,y 原点左下、y 向上；w,h 为参照系尺寸
 *
 * 关键映射：
 *   数位板 ABS_X 0..40640 → 容器 x 0..W
 *   数位板 ABS_Y 0..25400 → 容器 y（翻转成从底部量）
 *   ABS_PRESSURE 0..16383 → 0..pmax
 *   BTN_TOUCH 决定"按下"，抬起时压力强制为 0
 */
public final class PenSender {

    private static final int EV_SYN = 0;
    private static final int EV_KEY = 1;
    private static final int EV_ABS = 3;

    private static final int SYN_REPORT = 0;

    private static final int ABS_X = 0x00;
    private static final int ABS_Y = 0x01;
    private static final int ABS_PRESSURE = 0x18;

    private static final int BTN_TOUCH = 0x14a;
    private static final int BTN_STYLUS = 0x14b;
    private static final int BTN_TOOL_RUBBER = 0x141;

    private static final int TAB_X_MAX = 40640;
    private static final int TAB_Y_MAX = 25400;
    private static final int TAB_P_MAX = 16383;

    private static final int SAMPLE_PORT = 47800;

    public static void main(String[] args) throws Exception {
        String dev = args.length > 0 ? args[0] : "/dev/input/event8";
        int W = args.length > 1 ? Integer.parseInt(args[1]) : 1280;
        int H = args.length > 2 ? Integer.parseInt(args[2]) : 720;
        int PMAX = args.length > 3 ? Integer.parseInt(args[3]) : 4095;
        int HZ = args.length > 4 ? Integer.parseInt(args[4]) : 250;
        // 用于注入"按下/抬起"时把容器坐标换算回 Android 屏幕坐标
        int DISP_W = args.length > 5 ? Integer.parseInt(args[5]) : 2400;  // 横屏长边
        int DISP_H = args.length > 6 ? Integer.parseInt(args[6]) : 1080;  // 横屏短边
        boolean INJECT = args.length <= 7 || !"0".equals(args[7]);

        System.out.println("PenSender: dev=" + dev + " screen=" + W + "x" + H
                + " pmax=" + PMAX + " maxHz=" + HZ);

        DatagramSocket sock = new DatagramSocket();
        InetAddress loop = InetAddress.getByName("127.0.0.1");
        FileInputStream in = new FileInputStream(dev);

        final int EV_SIZE = 24;              // struct input_event on 64-bit
        byte[] buf = new byte[EV_SIZE];

        int x = 0, y = 0, press = 0;
        boolean tip = false, havePos = false, eraser = false, barrel = false;
        int seq = 1;
        long sent = 0;
        long lastNs = 0;
        long minGapNs = 1_000_000_000L / Math.max(1, HZ);
        long t0 = System.currentTimeMillis();

        while (true) {
            int n = in.read(buf, 0, EV_SIZE);
            if (n < EV_SIZE) continue;

            int type = u16(buf, 16);
            int code = u16(buf, 18);
            int value = s32(buf, 20);

            if (type == EV_ABS) {
                switch (code) {
                    case ABS_X: x = value; havePos = true; break;
                    case ABS_Y: y = value; havePos = true; break;
                    case ABS_PRESSURE: press = value; break;
                    default: break;
                }
            } else if (type == EV_KEY) {
                if (code == BTN_TOUCH) {
                    boolean wasTip = tip;
                    tip = value != 0;
                    // 笔尖状态变化 → 注入鼠标按下/抬起，让容器里的 SAI 真正"开始一笔"
                    if (INJECT && wasTip != tip) {
                        int sx0 = (int) Math.round(x * (double) DISP_W / TAB_X_MAX);
                        int sy0 = (int) Math.round(y * (double) DISP_H / TAB_Y_MAX);
                        inject(tip ? "DOWN" : "UP", sx0, sy0);
                    }
                }
                else if (code == BTN_STYLUS) barrel = value != 0;
                else if (code == BTN_TOOL_RUBBER) eraser = value != 0;
            } else if (type == EV_SYN && code == SYN_REPORT) {
                if (!havePos) continue;

                long now = System.nanoTime();
                if (now - lastNs < minGapNs) continue;   // 限速
                lastNs = now;

                int sx = (int) Math.round(x * (double) W / TAB_X_MAX);
                int sy = (int) Math.round(y * (double) H / TAB_Y_MAX);
                if (sx < 0) sx = 0; else if (sx >= W) sx = W - 1;
                if (sy < 0) sy = 0; else if (sy >= H) sy = H - 1;

                int p = (int) Math.round(press * (double) PMAX / TAB_P_MAX);
                if (p < 0) p = 0; else if (p > PMAX) p = PMAX;
                if (!tip) p = 0;                  // 抬笔一律 0
                else if (p < 1) p = 1;            // 接触了就别报 0

                int wireY = H - sy;               // 报文 y 从底部量

                String s = seq + " " + p + " " + sx + " " + wireY + " " + W + " " + H;
                byte[] d = s.getBytes("US-ASCII");
                sock.send(new DatagramPacket(d, d.length, loop, SAMPLE_PORT));

                seq++;
                sent++;
                if ((sent & 1023) == 0) {
                    System.err.println("sent=" + sent + " up=" + ((System.currentTimeMillis() - t0) / 1000)
                            + "s tip=" + tip + " eraser=" + eraser + " barrel=" + barrel);
                }
            }
        }
    }

    /**
     * 注入一次鼠标按下/抬起。用 input 命令实现，进程启动约几十毫秒，
     * 但每笔只有两次（起笔、收笔），可以接受。
     * 需要容器里的 Winlator 已打 penmode 补丁：按下即按住左键。
     */
    private static void inject(String action, int ax, int ay) {
        try {
            String cmd = "input motionevent " + action + " " + ax + " " + ay;
            Runtime.getRuntime().exec(new String[]{"/system/bin/sh", "-c", cmd});
        } catch (Throwable t) {
            System.err.println("inject failed: " + t);
        }
    }

    private static int u16(byte[] b, int o) {
        return (b[o] & 0xff) | ((b[o + 1] & 0xff) << 8);
    }

    private static int s32(byte[] b, int o) {
        return (b[o] & 0xff) | ((b[o + 1] & 0xff) << 8)
                | ((b[o + 2] & 0xff) << 16) | ((b[o + 3] & 0xff) << 24);
    }
}
