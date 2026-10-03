plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
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
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    // The decompiled engine does not use exceptions or RTTI
                    // and both cost size and a little speed.
                    "-DANDROID_CPP_FEATURES=",
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DKISAK_BUILD_MP=ON"
                )
                cppFlags += listOf("-std=c++20")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../../../CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
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
