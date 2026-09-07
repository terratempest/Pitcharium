# Pitcharium

Android instrument tuner with live chromatic spectrogram and radial visualizations. Kotlin and Jetpack Compose provide the interface; a native C++ engine processes microphone audio on the device.

## Features

- Live pitch, note, octave, and cents deviation.
- Spectrogram, radial, and instrument tuning views.
- Adjustable A4 reference, sample rate, and scrolling speed.
- Room-noise calibration and optional noise suppression.
- On-device performance display.

Microphone permission is required. The app processes audio locally and does not require an account or API key.

## Download

Download the signed APK from [GitHub Releases](https://github.com/terratempest/Pitcharium/releases). Android 7.0 or newer is required. Allow installation from your browser or file manager when Android prompts you.

## Build

Requires Android Studio's JDK (17 or newer), Android SDK platform 36.1, and CMake 3.22.1. Gradle uses the included wrapper; install the SDK/NDK components requested by Android Studio or Gradle.

Open this directory in Android Studio and let it create `local.properties`, or set your local SDK path there. This file is ignored by Git.

```powershell
$env:JAVA_HOME = '<Android Studio JDK directory>'
.\gradlew.bat :app:assembleDebug
```

On macOS/Linux, use `./gradlew :app:assembleDebug` with `JAVA_HOME` set to your JDK.

Debug APK: `app/build/outputs/apk/debug/app-debug.apk`.

The application ID is `com.pitcharium`. Install on a selected device with:

```text
adb -s <device-serial> install -r app/build/outputs/apk/debug/app-debug.apk
adb -s <device-serial> shell am start -n com.pitcharium/.TunerActivity
```
