plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Repository root: this file is ports/android/launcher/app/build.gradle.kts.
val repoRoot: File = rootProject.projectDir.resolve("../../..").canonicalFile

// Each engine mode is a full compile of ~600 translation units, and SP and MP
// cannot share a binary (KISAK_MP changes struct layouts). Building only SP
// halves the build, which is the difference between a CI run that finishes
// and one that times out.
val buildMultiplayer: Boolean = (findProperty("kisak.mp") as String? ?: "true").toBoolean()

// Opt-in because a machine without ccache on PATH would fail at configure
// time with a confusing "CMAKE_CXX_COMPILER_LAUNCHER not found".
val useCcache: Boolean =
    (findProperty("kisak.ccache") as String? ?: System.getenv("KISAK_CCACHE") ?: "false").toBoolean()

// The CMake the SDK provides. Overridable because the pinned one occasionally
// has to move ahead of an NDK; the CI workflow installs this exact version.
val sdkCmakeVersion: String = (findProperty("kisak.cmake") as String?) ?: "3.22.1"

val nativeArguments: List<String> = buildList {
    add("-DANDROID_STL=c++_static")
    // The decompiled engine does not use exceptions or RTTI and both cost
    // size and a little speed.
    add("-DANDROID_CPP_FEATURES=")
    // Not RelWithDebInfo, which is what AGP would pass: the release flags in
    // ports/android/CMakeLists.txt are guarded on $<CONFIG:Release>, so -O2
    // and --gc-sections would silently not apply.
    add("-DCMAKE_BUILD_TYPE=Release")
    val mp = if (buildMultiplayer) "ON" else "OFF"
    add("-DKISAK_BUILD_MP=$mp")

    // OpenAL Soft is vendored at the same version the FetchContent fallback
    // would clone. Using the checkout keeps the build offline and stops the
    // tag and the working tree drifting apart.
    val openal = repoRoot.resolve("third-party/openal-soft")
    if (openal.resolve("CMakeLists.txt").isFile) {
        add("-DFETCHCONTENT_SOURCE_DIR_OPENAL=" + openal.invariantSeparatorsPath)
    }

    if (useCcache) {
        add("-DCMAKE_C_COMPILER_LAUNCHER=ccache")
        add("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
    }
}

android {
    namespace = "ovh.kisak.cod4"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "ovh.kisak.cod4"
        // 26 is where AAudio, the full Vulkan loader behaviour and
        // ANativeWindow_setFrameRate's prerequisites all exist. Below that the
        // port would need fallbacks for devices that cannot hold 60 fps
        // anyway.
        minSdk = 26
        targetSdk = 35
        versionCode = 4
        versionName = "1.0.3"

        // arm64 only. The engine is a 64-bit port of 32-bit Windows code;
        // building it 32-bit would reintroduce exactly the pointer-size bugs
        // the port exists to fix.
        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments += nativeArguments
                cppFlags += listOf("-std=c++20")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../../../CMakeLists.txt")
            version = sdkCmakeVersion
        }
    }

    // An unsigned release APK cannot be installed, which makes it useless as a
    // CI artifact. With the four KISAK_* variables set the APK is signed for
    // real; without them it falls back to the debug key, which is enough to
    // sideload and play but changes between machines - uninstall the previous
    // build before installing one signed by a different key.
    signingConfigs {
        val keystore = System.getenv("KISAK_KEYSTORE")
        if (!keystore.isNullOrBlank()) {
            create("release") {
                storeFile = file(keystore)
                storePassword = System.getenv("KISAK_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("KISAK_KEY_ALIAS")
                keyPassword = System.getenv("KISAK_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // Symbols are kept out of the APK but uploaded alongside it, so a
            // tombstone from a player's device can still be symbolicated.
            ndk {
                debugSymbolLevel = "FULL"
            }
        }
        debug {
            isJniDebuggable = true
            // Keeps the debug and release installs side by side, which makes
            // "does this reproduce on a clean build" answerable without
            // uninstalling.
            applicationIdSuffix = ".debug"
        }
    }

    packaging {
        jniLibs {
            // The engine dlopens nothing and the libraries are large; leaving
            // them uncompressed lets the loader map them straight from the
            // APK instead of extracting to /data at install time.
            useLegacyPackaging = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        viewBinding = false
    }

    sourceSets {
        getByName("main") {
            java.srcDirs("src/main/kotlin")
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("com.google.android.material:material:1.12.0")
}
