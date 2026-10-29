plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}
android {
    namespace = "com.example.tool"
    compileSdk = 34
    defaultConfig {
        applicationId = "com.example.tool"
        minSdk = 27
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"
        externalNativeBuild { cmake { cppFlags += "-std=c++17"; arguments += "-DCMAKE_BUILD_TYPE=Release" } }
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a") }
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1" } }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
    packaging { jniLibs { useLegacyPackaging = true } }
}
dependencies { implementation("androidx.core:core-ktx:1.12.0") }
