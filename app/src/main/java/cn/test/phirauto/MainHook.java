package cn.test.phirauto;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.util.Log;
import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

public class MainHook implements IXposedHookLoadPackage {
    private static final String TAG = "PhiraAgent";

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lpp) {
        Log.i(TAG, "handleLoadPackage: " + lpp.packageName);
        try {
            XposedHelpers.findAndHookMethod("android.app.Instrumentation", lpp.classLoader,
                    "callApplicationOnCreate", Application.class, new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam param) {
                            Application app = (Application) param.args[0];
                            OverlayLog.log("app created: " + lpp.packageName);
                            Log.i(TAG, "application onCreate: " + lpp.packageName);
                            startAgent(app);
                        }
                    });
            XposedHelpers.findAndHookMethod("android.app.Instrumentation", lpp.classLoader,
                    "callActivityOnCreate", Activity.class, Bundle.class, new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam param) {
                            OverlayLog.attach((Activity) param.args[0]);
                        }
                    });
            Log.i(TAG, "hooks registered");
        } catch (Throwable t) {
            // Must not rely on XposedHelpers.log here — write straight to logcat.
            Log.e(TAG, "hook registration failed", t);
        }
    }

    private static volatile boolean armed = false;

    private static void startAgent(final Application app) {
        if (armed) return;
        armed = true;
        new Thread(() -> {
            int st;
            try {
                st = PhiraAgent.arm(app.getAssets());
            } catch (Throwable t) {
                Log.e(TAG, "arm threw", t);
                OverlayLog.log("arm exception: " + t);
                return;
            }
            OverlayLog.log("arm status=" + st + describe(st));
        }, "phira-agent").start();
    }

    private static String describe(int st) {
        switch (st) {
            case 1:  return " (注入成功)";
            case -1: return " 目标 so 未出现";
            case -2: return " 特征未匹配";
            case -3: return " 候选歧义，已放弃";
            default: return " 内部错误";
        }
    }
}
