plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "easylink.yvak.inc"
    compileSdk {
        version = release(36)
    }

    defaultConfig {
        applicationId = "easylink.yvak.inc"
        minSdk = 29
        targetSdk = 36
        versionCode = 14
        versionName = "1.7.6"
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
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.lifecycle.service)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.material.icons)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.osmdroid.android)
    implementation(libs.hms.wearengine)
    // Веб-мост: WebSocket к серверу LoRa-чата. OkHttp, а не
    // Socket.IO — на той стороне обычный сокет (см. WEB_BRIDGE.md).
    implementation(libs.okhttp)
    implementation(libs.zxing.core)
    implementation(libs.zxing.embedded)
    debugImplementation(libs.androidx.compose.ui.tooling)
}
