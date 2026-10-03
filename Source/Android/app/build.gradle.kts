import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    kotlin("plugin.serialization") version "2.2.21"
    id("androidx.baselineprofile")
}

@Suppress("UnstableApiUsage")
android {
    compileSdkVersion = "android-36"
    ndkVersion = "29.0.14206865"

    buildFeatures {
        viewBinding = true
        buildConfig = true
    }

    compileOptions {
        // Flag to enable support for the new language APIs
        isCoreLibraryDesugaringEnabled = true

        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlin {
        compilerOptions {
            jvmTarget = JvmTarget.fromTarget("17")
        }
    }

    lint {
        // This is important as it will run lint but not abort on error
        // Lint has some overly obnoxious "errors" that should really be warnings
        abortOnError = false

        //Uncomment disable lines for test builds...
        //disable "MissingTranslation"
        //disable "ExtraTranslation"
    }

    defaultConfig {
        applicationId = "org.dolphinemu.dolphinemu"
        minSdk = 21
        targetSdk = 36

        // Public Quest versions must not depend on a developer's private Git history.
        versionCode = 264

        versionName = "0.1.0-preview.1"

        buildConfigField("String", "GIT_HASH", "\"${getGitHash()}\"")
        buildConfigField("String", "BRANCH", "\"${getBranch()}\"")
        buildConfigField("boolean", "IS_QUEST", "false")

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    signingConfigs {
        create("release") {
            if (project.hasProperty("keystore")) {
                storeFile = file(project.property("keystore")!!)
                storePassword = project.property("storepass").toString()
                keyAlias = project.property("keyalias").toString()
                keyPassword = project.property("keypass").toString()
            }
        }
    }

    // Define build types, which are orthogonal to product flavors.
    buildTypes {
        // Signed by release key, allowing for upload to Play Store.
        release {
            if (project.hasProperty("keystore")) {
                signingConfig = signingConfigs.getByName("release")
            }

            resValue("string", "app_name_suffixed", "PrimedGun Quest")
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android.txt"),
                "proguard-rules.pro"
            )
        }

        // Signed by debug key disallowing distribution on Play Store.
        // Attaches "debug" suffix to version and package name, allowing installation alongside the release build.
        debug {
            resValue("string", "app_name_suffixed", "PrimedGun Quest")
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
            isJniDebuggable = true
        }

        // Sideload preview: keep the prototype application ID and signing lineage,
        // but do not expose Java/native debugging in the distributed APK.
        create("preview") {
            initWith(getByName("debug"))
            applicationIdSuffix = ".debug"
            versionNameSuffix = ""
            isDebuggable = false
            isJniDebuggable = false
            matchingFallbacks += "debug"
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    flavorDimensions += "hardware"
    productFlavors {
        create("quest") {
            dimension = "hardware"
            applicationId = "org.primedgun.quest3"
            minSdk = 29
            targetSdk = 32
            buildConfigField("boolean", "IS_QUEST", "true")
            manifestPlaceholders["questSupportedDevices"] = "quest3|quest3s"
            ndk.abiFilters += "arm64-v8a"
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../../CMakeLists.txt")
            version = "3.22.1+"
        }
    }
    namespace = "org.dolphinemu.dolphinemu"

    defaultConfig {
        externalNativeBuild {
            cmake {
                arguments(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
                    // , "-DENABLE_GENERIC=ON"
                )
                abiFilters("arm64-v8a")

                targets("main", "hook_impl", "main_hook", "gsl_alloc_hook", "file_redirect_hook")
            }
        }
    }

    packaging {
        jniLibs.useLegacyPackaging = true
    }

    // Generate assets independently of CMake: AGP can merge APK assets before native configure.
    sourceSets.getByName("main").assets.setSrcDirs(listOf(layout.buildDirectory.dir("generated/primedGunAssets")))
}

val preparePrimedGunAssets by tasks.registering(Sync::class) {
    into(layout.buildDirectory.dir("generated/primedGunAssets"))
    from("../../../Data/Sys") {
        into("Sys")
        exclude("Themes/**")
    }
    // Optional game-derived artwork/texture packs are not distributed.
    from("../../../LICENSES") { into("Sys/Licenses/Project") }
    from("../../../COPYING") { into("Sys/Licenses/Project") }
}

// Building a sideload APK must not require a managed phone emulator or a headset.
// Profiles can still be generated explicitly by developers.
baselineProfile {
    automaticGenerationDuringBuild = false
}

// Release lint also reads the generated asset directory. Declare the producer
// for both packaging and lint so clean Gradle builds cannot race that copy.
tasks.matching {
    (it.name.startsWith("merge") && it.name.endsWith("Assets")) || it.name.contains("lint", ignoreCase = true)
}.configureEach {
    dependsOn(preparePrimedGunAssets)
}

dependencies {
    baselineProfile(project(":benchmark"))
    coreLibraryDesugaring("com.android.tools:desugar_jdk_libs:2.1.5")

    implementation("androidx.core:core-ktx:1.17.0")
    implementation("androidx.appcompat:appcompat:1.7.1")
    implementation("androidx.cardview:cardview:1.0.0")
    implementation("androidx.recyclerview:recyclerview:1.4.0")
    implementation("androidx.constraintlayout:constraintlayout:2.2.1")
    implementation("androidx.fragment:fragment-ktx:1.8.9")
    implementation("androidx.slidingpanelayout:slidingpanelayout:1.2.0")
    implementation("com.google.android.material:material:1.13.0")
    implementation("androidx.core:core-splashscreen:1.2.0")
    implementation("androidx.preference:preference-ktx:1.2.1")
    implementation("androidx.profileinstaller:profileinstaller:1.4.1")

    // Kotlin extensions for lifecycle components
    implementation("androidx.lifecycle:lifecycle-viewmodel-ktx:2.9.4")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.4")

    // Android TV UI libraries.
    implementation("androidx.leanback:leanback:1.2.0")
    implementation("androidx.tvprovider:tvprovider:1.1.0")
    implementation("androidx.swiperefreshlayout:swiperefreshlayout:1.1.0")

    // For loading game covers from disk and GameTDB
    implementation("io.coil-kt:coil:2.7.0")

    // For loading custom GPU drivers
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.9.0")

    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")

    implementation("com.nononsenseapps:filepicker:4.2.1")
}

fun getGitVersion(): String {
    try {
        return ProcessBuilder("git", "describe", "--always", "--long")
            .directory(project.rootDir)
            .redirectOutput(ProcessBuilder.Redirect.PIPE)
            .redirectError(ProcessBuilder.Redirect.PIPE)
            .start().inputStream.bufferedReader().use { it.readText() }
            .trim()
            .replace(Regex("(-0)?-[^-]+$"), "")
    } catch (e: Exception) {
        logger.error("Cannot find git, defaulting to dummy version number")
    }

    return "0.0"
}

fun getBuildVersionCode(): Int {
    try {
        val commitCount = Integer.valueOf(
            ProcessBuilder("git", "rev-list", "--first-parent", "--count", "HEAD")
                .directory(project.rootDir)
                .redirectOutput(ProcessBuilder.Redirect.PIPE)
                .redirectError(ProcessBuilder.Redirect.PIPE)
                .start().inputStream.bufferedReader().use { it.readText() }
                .trim()
        )

        val isRelease = ProcessBuilder("git", "describe", "--exact-match", "HEAD")
            .directory(project.rootDir)
            .redirectOutput(ProcessBuilder.Redirect.PIPE)
            .redirectError(ProcessBuilder.Redirect.PIPE)
            .start()
            .waitFor() == 0

        return commitCount * 2 + (if (isRelease) 0 else 1)
    } catch (e: Exception) {
        logger.error("Cannot find git, defaulting to dummy version code")
    }

    return 1
}

fun getGitHash(): String {
    try {
        return ProcessBuilder("git", "rev-parse", "HEAD")
            .directory(project.rootDir)
            .redirectOutput(ProcessBuilder.Redirect.PIPE)
            .redirectError(ProcessBuilder.Redirect.PIPE)
            .start().inputStream.bufferedReader().use { it.readText() }
            .trim()
    } catch (e: Exception) {
        logger.error("Cannot find git, defaulting to dummy git hash")
    }

    return "0"
}

fun getBranch(): String {
    try {
        return ProcessBuilder("git", "rev-parse", "--abbrev-ref", "HEAD")
            .directory(project.rootDir)
            .redirectOutput(ProcessBuilder.Redirect.PIPE)
            .redirectError(ProcessBuilder.Redirect.PIPE)
            .start().inputStream.bufferedReader().use { it.readText() }
            .trim()
    } catch (e: Exception) {
        logger.error("Cannot find git, defaulting to dummy git hash")
    }

    return "master"
}
