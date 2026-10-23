"""Supported Gangstar Rio Android binary profile."""

PACKAGE = "com.gameloft.android.ANMP.GloftG4HM"
APK_SO_PATH = "lib/armeabi-v7a/libgl2jni.so"
SO_NAME = "libgl2jni.so"
SO_SIZE = 15_331_160
SO_SHA256 = "c7268efa9cc93da048bc44c5a319531bc6b72668a764b779d17c71bff845cb94"
MAIN_OBB_NAME = "main.12030.com.gameloft.android.ANMP.GloftG4HM.obb"
PATCH_OBB_NAME = "patch.12221.com.gameloft.android.ANMP.GloftG4HM.obb"

EXPECTED_NEEDED = {
    "libOpenSLES.so",
    "libGLESv2.so",
    "libdl.so",
    "liblog.so",
    "libm.so",
    "libc.so",
    "libz.so",
    "libstdc++.so",
}
