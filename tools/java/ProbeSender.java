import com.winlator.sai.SaiPressureBridge;

/**
 * 压感桥的"驱动器"：把一组已知的屏幕坐标喂给 SaiPressureBridge，
 * 并把每个样本打印成一行 {@code S x y p}，供 tools/verify_bridge.py 逐条比对。
 *
 * 用法（由 verify_bridge.py 自动编译运行）：
 *   java -cp <classes> ProbeSender [width] [height]
 *
 * 它本身不是产品代码，只是把"发送端 → 报文"这段变成可断言的。
 */
public final class ProbeSender {

    public static void main(String[] args) throws Exception {
        int w = args.length > 0 ? Integer.parseInt(args[0]) : 1600;
        int h = args.length > 1 ? Integer.parseInt(args[1]) : 900;

        if (!SaiPressureBridge.configure(w, h)) {
            System.err.println("configure failed");
            System.exit(2);
        }
        Thread.sleep(60);            // 等发送线程起来

        // 四角 + 中心：足以暴露任何轴翻转/偏移
        float[][] pts = {
            {0f, 0f, 0.10f},
            {w - 1f, 0f, 0.25f},
            {0f, h - 1f, 0.50f},
            {w - 1f, h - 1f, 0.75f},
            {w / 2f, h / 2f, 1.00f},
        };

        for (float[] pt : pts) emit(pt[0], pt[1], pt[2]);

        // 一个圆：24 个点，压力 0.7，用来验证比例映射在中间区域也成立
        float cx = w / 2f, cy = h / 2f, rx = w * 0.30f, ry = h * 0.30f;
        for (int i = 0; i < 24; i++) {
            double t = 2.0 * Math.PI * i / 24.0;
            emit((float) (cx + rx * Math.cos(t)), (float) (cy + ry * Math.sin(t)), 0.70f);
        }

        // 收笔
        emit(w / 2f, h / 2f, 0f, false);

        Thread.sleep(150);
        SaiPressureBridge.shutdown();
    }

    private static void emit(float x, float y, float p) throws Exception {
        emit(x, y, p, p > 0f);
    }

    private static void emit(float x, float y, float p, boolean down) throws Exception {
        System.out.println("S " + x + " " + y + " " + p);
        System.out.flush();
        if (down) SaiPressureBridge.onPenSample(x, y, p, true);
        else      SaiPressureBridge.onPenUp(x, y);
        Thread.sleep(25);
    }
}
