package de.robv.android.xposed;

// Compile-time stub; the real class is provided by LSPosed at runtime.
public final class XposedHelpers {

    private XposedHelpers() {
    }

    public static void log(String text) {
    }

    public static void log(Throwable t) {
    }

    public static XC_MethodHook.Unhook findAndHookMethod(
            String className, ClassLoader classLoader, String methodName,
            Object... parameterTypesAndCallback) {
        throw new UnsupportedOperationException("stub");
    }
}
