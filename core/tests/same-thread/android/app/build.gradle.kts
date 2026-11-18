plugins {
    id("com.android.application")
}
val testOutput = rootProject.projectDir
    .resolve("../../../build/tests/same-thread")
    .canonicalFile
layout.buildDirectory.set(testOutput.resolve("android-app"))

android {
    namespace = "dev.glibc.worker.domains"
    compileSdk = 36
    defaultConfig {
        applicationId = "dev.glibc.worker.domains"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "0.1-test"
        ndk {
            abiFilters += "arm64-v8a"
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    sourceSets.getByName("main") {
        jniLibs.srcDir(testOutput.resolve("android-jniLibs"))
    }
    packaging {
        jniLibs {
            useLegacyPackaging = true
            keepDebugSymbols += "**/*.so"
        }
    }
}
