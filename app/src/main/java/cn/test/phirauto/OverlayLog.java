package cn.test.phirauto;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Locale;

/**
 * In-app floating log panel. Lives inside the host activity's decor view —
 * no system overlay window, no SYSTEM_ALERT_WINDOW permission needed.
 * Logs are buffered until the first activity appears, then flushed.
 */
public final class OverlayLog {
    private static final ArrayDeque<String> BUF = new ArrayDeque<>();
    private static final SimpleDateFormat FMT = new SimpleDateFormat("HH:mm:ss.SSS", Locale.US);
    private static volatile TextView body;
    private static volatile boolean attachedToCurrent;

    private OverlayLog() {}

    /** Call from Instrumentation.callActivityOnCreate (main thread). */
    public static void attach(Activity activity) {
        try {
            ViewGroup decor = (ViewGroup) activity.getWindow().getDecorView();
            View existing = decor.findViewWithTag("phira_agent_overlay");
            if (existing != null) return;
            LinearLayout panel = buildPanel(activity);
            decor.addView(panel, new android.widget.FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    dp(activity, 240),
                    Gravity.TOP));
            attachedToCurrent = true;
            flush();
            log("overlay attached to " + activity.getClass().getSimpleName());
        } catch (Throwable t) {
            android.util.Log.e("PhiraAgent", "overlay attach failed", t);
        }
    }

    public static void log(String msg) {
        String line = FMT.format(new Date()) + "  " + msg;
        synchronized (BUF) {
            BUF.addLast(line);
            while (BUF.size() > 400) BUF.pollFirst();
        }
        TextView v = body;
        if (v != null) v.post(() -> flush());
    }

    private static void flush() {
        TextView v = body;
        if (v == null) return;
        StringBuilder sb = new StringBuilder();
        synchronized (BUF) {
            for (String s : BUF) sb.append(s).append('\n');
        }
        v.setText(sb.toString());
        ((ScrollView) v.getParent()).fullScroll(View.FOCUS_DOWN);
    }

    private static LinearLayout buildPanel(Activity activity) {
        LinearLayout panel = new LinearLayout(activity);
        panel.setTag("phira_agent_overlay");
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(0xD8101010);
        int pad = dp(activity, 6);
        panel.setPadding(pad, pad, pad, pad);

        LinearLayout bar = new LinearLayout(activity);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);

        TextView title = new TextView(activity);
        title.setText("PhiraAgent 日志");
        title.setTextColor(Color.WHITE);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextSize(12f);
        bar.addView(title, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        Button close = new Button(activity);
        close.setText("关闭");
        close.setTextSize(11f);
        close.setMinHeight(0);
        close.setMinWidth(0);
        close.setOnClickListener(v -> {
            ViewGroup parent = (ViewGroup) panel.getParent();
            if (parent != null) parent.removeView(panel);
            body = null;
            attachedToCurrent = false;
        });
        bar.addView(close, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        ScrollView scroll = new ScrollView(activity);
        TextView text = new TextView(activity);
        text.setTextColor(Color.WHITE);
        text.setTextSize(9f);
        text.setTypeface(Typeface.MONOSPACE);
        body = text;
        scroll.addView(text);
        scroll.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        panel.addView(bar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        panel.addView(scroll);

        return panel;
    }

    private static int dp(Activity a, int v) {
        return Math.round(v * a.getResources().getDisplayMetrics().density);
    }
}
