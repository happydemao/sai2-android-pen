package com.winlator.sai;

import android.os.SystemClock;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * 把 SAI 真正干活的线程放到大核上（借鉴 SAI Pen for Android 的 SaiCores）。
 *
 * 不把所有线程都塞进大核：SAI 落笔时的工作分散在多个线程里，全塞会互相
 * 抢核，反而更慢。这里每秒看一次 /proc/<pid>/task 的 CPU 时间，只在落笔
 * 期间累计，按负载挑线程，并且总负载不超过大核数 * 0.9。
 *
 * Wine 在 SAI 窗口出现时会把整个进程的亲和性重设成所有核，所以选中的
 * 线程每秒重新 taskset 一次。
 */
final class CoreTuner {
    private static final String TAG = "SaiCoreTuner";
    private static final long INTERVAL_MS = 1000;
    private static final double MIN_LOAD = 0.03;   // 至少 3% 的一个核
    private static final double ROOM = 0.90;       // 最多占大核总容量的 90%
    private static final long WARM_UP_MS = 2500;
    private static boolean started;

    private CoreTuner() { }

    static synchronized void start() {
        if (started) return;
        started = true;
        try {
            Thread t = new Thread(new Runnable() {
                @Override public void run() { loop(); }
            }, "SaiCoreTuner");
            t.setDaemon(true);
            t.setPriority(Thread.MIN_PRIORITY);
            t.start();
        } catch (Throwable t) {
            Log.w(TAG, "start failed: " + t);
        }
    }

    private static void loop() {
        int[] fast = fastCores();
        if (fast.length == 0) {
            log("no slow cores / no capacity info; core tuning off");
            return;
        }
        long fastMask = mask(fast);
        long allMask = mask(allCores());
        if (fastMask == 0 || allMask == 0) return;
        log("fast cores " + range(fast) + " mask=0x" + Long.toHexString(fastMask));

        int pid = -1;
        Set<Integer> pinned = new HashSet<Integer>();
        Map<Integer, Long> last = new HashMap<Integer, Long>();
        Map<Integer, Long> drawTicks = new HashMap<Integer, Long>();
        long drawMs = 0;
        long lastAt = -1;

        while (true) {
            try {
                Thread.sleep(INTERVAL_MS);
                if (pid < 0 || !new File("/proc/" + pid).isDirectory()) {
                    pid = findSaiPid();
                    pinned.clear();
                    last.clear();
                    drawTicks.clear();
                    drawMs = 0;
                    lastAt = -1;
                    if (pid < 0) continue;
                    log("SAI pid=" + pid);
                }
                long now = SystemClock.uptimeMillis();
                boolean drawing = PenBridge.isDrawingRecently();
                Map<Integer, Long> ticks = threadTicks(pid);
                if (drawing && lastAt >= 0 && !last.isEmpty()) {
                    long elapsed = now - lastAt;
                    if (elapsed > 0) {
                        drawMs += elapsed;
                        for (Map.Entry<Integer, Long> e : ticks.entrySet()) {
                            Long before = last.get(e.getKey());
                            long d = e.getValue() - (before == null ? 0L : before);
                            if (d > 0) {
                                Long old = drawTicks.get(e.getKey());
                                drawTicks.put(e.getKey(), (old == null ? 0L : old) + d);
                            }
                        }
                        drawTicks.keySet().retainAll(ticks.keySet());
                    }
                }
                last.clear();
                last.putAll(ticks);
                lastAt = now;

                if (drawMs < WARM_UP_MS) continue;
                List<Integer> pick = pick(drawTicks, drawMs, fast.length);
                if (pick.isEmpty()) continue;

                // 新选中的线程放大核；落选的回所有核
                for (int tid : pick) {
                    if (!pinned.contains(tid)) {
                        taskset(fastMask, tid);
                        log("pin tid " + tid + " load=" + load(drawTicks, drawMs, tid)
                                + " -> " + range(fast));
                    } else {
                        taskset(fastMask, tid);   // Wine 可能重置亲和性
                    }
                }
                for (int tid : new ArrayList<Integer>(pinned)) {
                    if (!pick.contains(tid)) {
                        taskset(allMask, tid);
                        log("unpin tid " + tid);
                    }
                }
                pinned.clear();
                pinned.addAll(pick);
            } catch (InterruptedException e) {
                return;
            } catch (Throwable t) {
                log("loop: " + t);
            }
        }
    }

    private static List<Integer> pick(Map<Integer, Long> ticks, long drawMs, int fastCount) {
        List<Integer> all = new ArrayList<Integer>(ticks.keySet());
        Collections.sort(all, new Comparator<Integer>() {
            @Override public int compare(Integer a, Integer b) {
                int c = Long.compare(ticks.get(b), ticks.get(a));
                return c != 0 ? c : Integer.compare(a, b);
            }
        });
        List<Integer> out = new ArrayList<Integer>();
        double used = 0;
        for (int tid : all) {
            double l = load(ticks, drawMs, tid);
            if (l < MIN_LOAD) continue;
            if (!out.isEmpty() && used + l > fastCount * ROOM) continue;
            out.add(tid);
            used += l;
        }
        if (out.isEmpty() && !all.isEmpty()) out.add(all.get(0));
        return out;
    }

    private static double load(Map<Integer, Long> ticks, long drawMs, int tid) {
        Long v = ticks.get(tid);
        if (v == null || drawMs <= 0) return 0;
        return v * 10.0 / drawMs;   // Linux tick = 1/100 s, drawMs in ms
    }

    private static int findSaiPid() {
        File[] ps = new File("/proc").listFiles();
        if (ps == null) return -1;
        for (File p : ps) {
            String n = p.getName();
            if (n.isEmpty() || !Character.isDigit(n.charAt(0))) continue;
            String cmd = readSmall(new File(p, "cmdline")).toLowerCase();
            if (cmd.contains("sai2.exe") || cmd.contains("sai2_") || cmd.endsWith("sai2")) {
                return Integer.parseInt(n);
            }
        }
        return -1;
    }

    private static Map<Integer, Long> threadTicks(int pid) {
        Map<Integer, Long> out = new HashMap<Integer, Long>();
        File[] ts = new File("/proc/" + pid + "/task").listFiles();
        if (ts == null) return out;
        for (File t : ts) {
            String n = t.getName();
            if (n.isEmpty() || !Character.isDigit(n.charAt(0))) continue;
            String stat = readSmall(new File(t, "stat"));
            int close = stat.lastIndexOf(')');
            if (close < 0) continue;
            String[] f = stat.substring(close + 2).trim().split("\\s+");
            if (f.length < 13) continue;
            try {
                long utime = Long.parseLong(f[11]);
                long stime = Long.parseLong(f[12]);
                out.put(Integer.parseInt(n), utime + stime);
            } catch (NumberFormatException ignored) { }
        }
        return out;
    }

    private static int[] fastCores() {
        int[] all = allCores();
        if (all.length == 0) return new int[0];
        long[] cap = new long[all.length];
        long max = 0;
        boolean any = false;
        for (int i = 0; i < all.length; i++) {
            cap[i] = readLong("/sys/devices/system/cpu/cpu" + all[i] + "/cpu_capacity");
            if (cap[i] <= 0) {
                cap[i] = readLong("/sys/devices/system/cpu/cpu" + all[i]
                        + "/cpufreq/cpuinfo_max_freq");
            }
            if (cap[i] > 0) any = true;
            max = Math.max(max, cap[i]);
        }
        if (!any || max <= 0) return new int[0];
        List<Integer> fast = new ArrayList<Integer>();
        for (int i = 0; i < all.length; i++) {
            if (cap[i] >= max * 0.60) fast.add(all[i]);
        }
        if (fast.size() == all.length || fast.isEmpty()) return new int[0];
        int[] out = new int[fast.size()];
        for (int i = 0; i < out.length; i++) out[i] = fast.get(i);
        return out;
    }

    private static int[] allCores() {
        File[] cs = new File("/sys/devices/system/cpu").listFiles();
        if (cs == null) return new int[0];
        List<Integer> ids = new ArrayList<Integer>();
        for (File c : cs) {
            String n = c.getName();
            if (!n.startsWith("cpu") || n.length() <= 3) continue;
            try { ids.add(Integer.parseInt(n.substring(3))); }
            catch (NumberFormatException ignored) { }
        }
        Collections.sort(ids);
        int[] out = new int[ids.size()];
        for (int i = 0; i < out.length; i++) out[i] = ids.get(i);
        return out;
    }

    private static long mask(int[] cores) {
        long m = 0;
        for (int c : cores) if (c >= 0 && c < 64) m |= 1L << c;
        return m;
    }

    private static String range(int[] cores) {
        if (cores.length == 0) return "[]";
        StringBuilder b = new StringBuilder("[");
        for (int i = 0; i < cores.length; i++) {
            if (i > 0) b.append(',');
            b.append(cores[i]);
        }
        return b.append(']').toString();
    }

    private static void taskset(long mask, int tid) {
        String bin = tasksetBin();
        if (bin == null) return;
        String m = Long.toHexString(mask);
        String id = Integer.toString(tid);
        try {
            Process p = new ProcessBuilder(bin, "-pc", m, id)
                    .redirectErrorStream(true).start();
            if (p.waitFor() == 0) return;
        } catch (Throwable ignored) { }
        try {
            Process p = new ProcessBuilder(bin, "-p", m, id)
                    .redirectErrorStream(true).start();
            p.waitFor();
        } catch (Throwable ignored) { }
    }

    private static String tasksetBin() {
        String[] paths = {
                "/system/bin/taskset",
                "/data/data/com.winlator/files/rootfs/usr/bin/taskset",
                "/data/data/com.winlator/files/rootfs/bin/taskset"
        };
        for (String p : paths) {
            if (new File(p).isFile()) return p;
        }
        return null;
    }

    private static String readSmall(File f) {
        try {
            byte[] b = new byte[(int) Math.min(f.length(), 4096)];
            FileInputStream in = new FileInputStream(f);
            try {
                int n = in.read(b);
                return n <= 0 ? "" : new String(b, 0, n, StandardCharsets.UTF_8);
            } finally { in.close(); }
        } catch (Throwable t) {
            return "";
        }
    }

    private static long readLong(String path) {
        try {
            String s = readSmall(new File(path)).trim();
            return s.isEmpty() ? -1 : Long.parseLong(s);
        } catch (Throwable t) {
            return -1;
        }
    }

    private static void log(String s) {
        try { Log.i(TAG, s); } catch (Throwable ignored) { }
        try {
            java.io.FileWriter w = new java.io.FileWriter(
                    "/data/data/com.winlator/files/pb_perf.log", true);
            try { w.write(s + "\n"); } finally { w.close(); }
        } catch (Throwable ignored) { }
    }
}
