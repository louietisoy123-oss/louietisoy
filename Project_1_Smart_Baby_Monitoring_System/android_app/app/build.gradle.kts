plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.msuiit.bca180.babymonitor"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.msuiit.bca180.babymonitor"
        minSdk = 24
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.recyclerview:recyclerview:1.3.2")

    // HTTP polling of the P5 PC backend (GET /api/latest, /api/events, ...)
    implementation("com.squareup.okhttp3:okhttp:4.12.0")

    // Plain JVM unit tests for the JSON response parsers (no Robolectric).
    // org.json on the JVM mirrors the android.jar org.json API.
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")
}
