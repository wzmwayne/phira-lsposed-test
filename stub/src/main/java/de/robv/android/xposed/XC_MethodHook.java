package de.robv.android.xposed;

// Compile-time stub; the real class is provided by LSPosed at runtime.
public abstract class XC_MethodHook {

    protected void beforeHookedMethod(MethodHookParam param) throws Throwable {
    }

    protected void afterHookedMethod(MethodHookParam param) throws Throwable {
    }

    public static class MethodHookParam {
        public Object thisObject;
        public Object[] args;
    }

    public static class Unhook {
        Unhook() {
        }

        public void unhook() {
        }
    }
}
