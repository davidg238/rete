plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}
android {
    namespace = "net.rete.relay"
    compileSdk = 35
    defaultConfig {
        applicationId = "net.rete.relay"
        minSdk = 34
        targetSdk = 35
        versionCode = 1
        versionName = "0.1-spike439"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}
dependencies {
    testImplementation("junit:junit:4.13.2")
}
// FrameTest reads the golden vector from protocol/ (rete#439). Without this input Gradle
// calls the test task up to date after the vector changes, and a broken vector reads green.
tasks.withType<Test>().configureEach {
    inputs.file(rootProject.file("../../protocol/fixtures/relay/frame.hex"))
}
