plugins {
    id("com.android.application")
}

val workspaceRoot = rootProject.projectDir.resolve("../../..").canonicalFile
val workerAbi = providers.environmentVariable("GW_ABI").orElse("x86_64").get()
require(workerAbi in setOf("x86_64", "arm64-v8a"))
layout.buildDirectory.set(workspaceRoot.resolve("jvm-jni/build/tests/android/app"))

android {
    namespace = "dev.glibc.worker"
    compileSdk = 36
    defaultConfig {
        applicationId = "dev.glibc.worker"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "0.1-spike"
        ndk {
            abiFilters += workerAbi
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets.getByName("main") {
        jniLibs.srcDir(workspaceRoot.resolve("jvm-jni/build/tests/android/jniLibs"))
        assets.srcDir(workspaceRoot.resolve("jvm-jni/build/tests/android/assets"))
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
            keepDebugSymbols += "**/*.so"
        }
    }

    buildFeatures {
        buildConfig = false
    }
}

val prepareNative by tasks.registering(Exec::class) {
    workingDir(workspaceRoot)
    commandLine("bash", "jvm-jni/tests/scripts/build-native.sh")

    inputs.property("workerAbi", workerAbi)
    inputs.property("workerContexts", providers.environmentVariable("GW_CONTEXTS").orElse("16"))
    inputs.property("bindingBackend", providers.environmentVariable("GW_BINDING_BACKEND").orElse("zig"))
    inputs.property("rpcProfile", providers.environmentVariable("GW_RPC_PROFILE").orElse("0"))
    inputs.property("rpcSpinNs", providers.environmentVariable("GW_RPC_SPIN_NS").orElse("20000"))
    inputs.property("rpcReadyPoll", providers.environmentVariable("GW_RPC_READY_POLL").orElse("0"))
    inputs.property("nativeJars", providers.environmentVariable("GW_NATIVE_JARS").orElse("0"))
    inputs.property("wakeFilter", providers.environmentVariable("GW_FUTEX_WAKE_FILTER").orElse("0"))

    val sourceDirectories = listOf(
        "core",
        "bionic-dlopen",
        "android-jni",
        "jvm-jni/src/native",
        "jvm-jni/src/java",
        "jvm-jni/tests/native",
        "jvm-jni/tests/java",
        "jvm-jni/tests/config",
        "jvm-jni/tests/third_party",
        "scripts",
        "jvm-jni/tests/scripts",
    )
    for (directory in sourceDirectories) {
        inputs.files(fileTree(workspaceRoot.resolve(directory)) {
            exclude("**/build/**", "**/work/**", "**/vendor/**", "**/.gradle/**")
        })
    }

    outputs.dir(workspaceRoot.resolve("jvm-jni/build/tests/android/jniLibs/$workerAbi"))
    outputs.dir(workspaceRoot.resolve("jvm-jni/build/tests/android/assets"))
}

tasks.named("preBuild") {
    dependsOn(prepareNative)
}
