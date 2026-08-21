package cn.test.phirauto;

import android.content.res.AssetManager;

public final class PhiraAgent {

    static {
        System.loadLibrary("phira_agent");
    }

    private PhiraAgent() {
    }

    /**
     * Blocks until libphira.so is located and patched, or until timeout.
     * Returns true only when a branch was actually rewritten.
     */
    public static native boolean arm(AssetManager assets);
}
