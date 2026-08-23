package cn.test.phirauto;

import android.content.res.AssetManager;

public final class PhiraAgent {

    static {
        System.loadLibrary("phira_agent");
    }

    private PhiraAgent() {
    }

    /**
     * Blocks until the target library is located and patched, or until timeout.
     * Returns a status code: 1 = patched; negative = failure reason.
     */
    public static native int arm(AssetManager assets);

    /** Called from native to mirror progress onto the overlay panel. */
    public static void report(String line) {
        OverlayLog.log("[agent] " + line);
    }

    /** Called from native to update the short status label in the title bar. */
    public static void reportStatus(String line) {
        OverlayLog.status(line);
    }
}
