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

    // Target selection is driven by the LSPosed scope (check your Phira build there).
    // No hard-coded package gate: every scoped process reports and probes for libphira.so.

    private static final AtomicBoolean FIRED = new AtomicBoolean(false);

    @Override
    public void handleLoadPackage(final XC_LoadPackage.LoadPackageParam lpp) {
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
                            toast(app, "模块已加载: " + lpp.packageName);
                            new Thread(new Runnable() {
                                @Override
                                public void run() {
                                    int st;
                                    try {
                                        st = PhiraAgent.arm(app.getAssets());
                                    } catch (Throwable t) {
                                        XposedHelpers.log(t);
                                        st = -100;
                                    }
                                    toast(app, describe(st));
                                }
                            }, "phira-agent").start();
                        }
                    });
        } catch (Throwable t) {
            XposedHelpers.log(t);
        }
    }

    private static String describe(int st) {
        switch (st) {
            case 1:
                return "Phira 方向1 注入成功";
            case -1:
                return "注入失败: 进程内未出现目标 so (检查作用域/库名)";
            case -2:
                return "注入失败: 特征未匹配 (logcat 过滤 PhiraAgent)";
            case -3:
                return "注入失败: 候选歧义已放弃";
            default:
                return "注入失败: 内部错误 st=" + st;
        }
    }

    private static void toast(final Application app, final String msg) {
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                Toast.makeText(app, msg, Toast.LENGTH_LONG).show();
            }
        });
    }
}
