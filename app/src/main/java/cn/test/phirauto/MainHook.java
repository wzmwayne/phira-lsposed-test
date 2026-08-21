package cn.test.phirauto;

import android.app.Application;
import android.os.Handler;
import android.os.Looper;
import android.widget.Toast;

import java.util.concurrent.atomic.AtomicBoolean;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

public class MainHook implements IXposedHookLoadPackage {

    // Self-built test builds ONLY — edit to match your applicationId.
    private static final String[] TARGET_PACKAGES = {
            "cn.mivik.phira",
            "com.mivik.phira",
    };

    private static final AtomicBoolean FIRED = new AtomicBoolean(false);

    @Override
    public void handleLoadPackage(final XC_LoadPackage.LoadPackageParam lpp) {
        if (!matches(lpp.packageName)) {
            return;
        }
        try {
            XposedHelpers.findAndHookMethod(
                    "android.app.Instrumentation", lpp.classLoader,
                    "callApplicationOnCreate", Application.class,
                    new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam param) {
                            if (!FIRED.compareAndSet(false, true)) {
                                return;
                            }
                            final Application app = (Application) param.args[0];
                            new Thread(new Runnable() {
                                @Override
                                public void run() {
                                    boolean ok;
                                    try {
                                        ok = PhiraAgent.arm(app.getAssets());
                                    } catch (Throwable t) {
                                        XposedHelpers.log(t);
                                        ok = false;
                                    }
                                    if (ok) {
                                        notifyInjected(app);
                                    }
                                }
                            }, "phira-agent").start();
                        }
                    });
        } catch (Throwable t) {
            XposedHelpers.log(t);
        }
    }

    private static boolean matches(String pkg) {
        for (String p : TARGET_PACKAGES) {
            if (p.equals(pkg)) {
                return true;
            }
        }
        return false;
    }

    private static void notifyInjected(final Application app) {
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                Toast.makeText(app, "Phira 方向1 注入成功", Toast.LENGTH_LONG).show();
            }
        });
    }
}
