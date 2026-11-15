plugins {
    id("com.android.application")
}
val experimentOutput = rootProject.projectDir
    .resolve("../../../build/experiments/same-thread")
    .canonicalFile
layout.buildDirectory.set(experimentOutput.resolve("android-app"))

android {
    namespace = "dev.glibc.worker.domains.hotspot"
    compileSdk = 36
    defaultConfig {
        applicationId = "dev.glibc.worker.domains.hotspot"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "0.1-experiment"
        ndk {
            abiFilters += "arm64-v8a"
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    sourceSets.getByName("main") {
        jniLibs.srcDir(experimentOutput.resolve("android-jniLibs"))
        assets.srcDir(experimentOutput.resolve("android-assets"))
    }
    packaging {
        jniLibs {
            useLegacyPackaging = true
            keepDebugSymbols += "**/*.so"
        }
    }
}
