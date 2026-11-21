package com.winlator.sai;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;

/**
 * The touch entry point for the liquify plugin.
 *
 * A Wine popup window is not usable here: under Winlator's window handling it
 * ends up behind the SAI window and often does not receive taps at all. This
 * button lives inside the app's own view hierarchy (added to the decor view of
 * the activity that owns the container view), so it is always above the
 * SurfaceView and always tappable.
 *
 * A tap drops C:\wt_ui into the container; the WinTab bridge watches that file
 * and opens the liquify overlay on the current document.
 */
public final class LiquifyButton {

    /* the container home as the app sees it (same path LiquifyServer uses) */
    private static final String DRIVE_C =
            "/data/data/lator.forsai/files/rootfs/home/xuser/.wine/drive_c";

    private static View sButton;

    private LiquifyButton() { }

    /** Called from TouchpadView.updateXform(int,int) with the view as host. */
    public static void install(final View host) {
        if (sButton != null || host == null) return;
        host.post(new Runnable() {
            @Override
            public void run() {
                addNow(host);
            }
        });
    }

    private static void addNow(View host) {
        if (sButton != null) return;
        try {
            Context ctx = host.getContext();
            View rootView = host.getRootView();
            if (ctx == null || !(rootView instanceof ViewGroup)) return;
            ViewGroup root = (ViewGroup) rootView;

            float d = ctx.getResources().getDisplayMetrics().density;
            TextView b = new TextView(ctx);
            b.setText("液化");
            b.setTextColor(Color.WHITE);
            b.setTextSize(14);
            b.setGravity(Gravity.CENTER);
            GradientDrawable bg = new GradientDrawable();
            bg.setColor(0xE62D7FF9);
            bg.setCornerRadius(30 * d);
            b.setBackground(bg);
            b.setElevation(6 * d);

            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                    (int) (60 * d), (int) (60 * d));
            lp.leftMargin = (int) (10 * d);
            lp.topMargin = (int) (84 * d);
            lp.gravity = Gravity.TOP | Gravity.LEFT;

            b.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    trigger();
                }
            });
            root.addView(b, lp);
            sButton = b;
        } catch (Throwable ignored) {
            /* never break the host app because of the button */
        }
    }

    private static void trigger() {
        try {
            File f = new File(DRIVE_C + "/wt_ui");
            FileOutputStream fo = new FileOutputStream(f);
            fo.write("go".getBytes("UTF-8"));
            fo.close();
        } catch (Throwable ignored) {
        }
    }
}
