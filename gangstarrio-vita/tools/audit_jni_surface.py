#!/usr/bin/env python3
"""Verify the JNI names recovered from the supported Rio ARM binary."""

from pathlib import Path
import re
import sys


REQUIRED_METHODS = {
    "CheckDoneAcceptPP", "clear", "CloseGame", "containsKey", "createView",
    "deleteSharedValue", "enableAccelerometer", "get", "getBoolean",
    "getByteArray", "getContentResolver", "getContext", "getDefault",
    "getDeviceId", "GetGangster4ButtonSwitch", "getGLiveUsername",
    "getGLiveUserpassword", "getHDIDFV", "getIdentifier", "getIMEI",
    "getInt", "getISO3Country", "getISO3Language", "getLocaleCountry",
    "getLong", "getMac", "GetMemoryInfo", "getMinBufferSize",
    "getNativeOutputSampleRate", "getObbFileName", "getObbPatchFileName",
    "getPackage", "getPhoneFW", "getPhoneManufacturer", "getPhoneModel",
    "getPlayState", "getPreferenceString", "getResource", "getSaveFolder",
    "getSDFolder", "getSharedValue", "getString", "getUserAgent",
    "getVersion", "InGameBrowser_OpenUrl", "InGameBrowser_SetBaseUrl",
    "isAndroidOrientationAvailable", "IsFirstLaunched", "IsPowerAConnected",
    "isSharedValue", "isSlideEnabled", "isUserMusicPlaying", "isWifiAlive",
    "isZEUSDevice", "launchCustomerCare", "LaunchFacebook", "launchG4Button",
    "launchGLLive", "LaunchTwitter", "launchWelcomeScr", "minimize",
    "nativeGetLangIngame", "NoBackWarning", "notifyTrophy", "openBrowser",
    "openIGP", "pause", "pauseUserMusic", "play", "putBoolean",
    "putByteArray", "putInt", "putLong", "putString", "randomUUID",
    "RateApp", "release", "sendGameTracking", "SetAndroidOrientation",
    "SetAutoOrientationOff", "SetAutoOrientationOn", "setClearFrames",
    "setCurrentContext", "setDisableKey", "SetDisableKey", "SetDoneAcceptPP",
    "setNoSwapFrames", "setProcessPositionEvents", "setPSKeyEnable",
    "setSharedValue", "setThreadPriority", "setupPaths", "setViewSettings",
    "startGame", "stop", "stopUserMusic", "toString", "UpdatePreferences",
    "UpdatePreferences2", "write",
}

REQUIRED_CONSTRUCTORS = {
    "android/os/Bundle/<init>",
    "android/media/AudioTrack/<init>",
}


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    source = (repo / "source/java.c").read_text(encoding="utf-8")
    registered = re.findall(
        r'\{\s*MID_[A-Z0-9_]+,\s*"([^"]+)",\s*METHOD_TYPE_[A-Z]+\s*\}',
        source,
    )
    names = set(registered)
    missing = sorted((REQUIRED_METHODS | REQUIRED_CONSTRUCTORS) - names)
    duplicates = sorted(name for name in names if registered.count(name) > 1)

    print(f"Recovered JNI methods: {len(REQUIRED_METHODS)}")
    print(f"Required constructors: {len(REQUIRED_CONSTRUCTORS)}")
    print(f"Registered bridge entries: {len(registered)}")
    if missing:
        print("Missing: " + ", ".join(missing))
    if duplicates:
        print("Ambiguous duplicate names: " + ", ".join(duplicates))
    if missing or duplicates:
        return 1
    print("JNI surface complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
