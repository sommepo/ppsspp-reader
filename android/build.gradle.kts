import com.google.protobuf.gradle.*
import org.gradle.process.ExecOperations
import java.io.ByteArrayOutputStream

plugins {
	id("com.android.application")
	id("com.google.protobuf")
}

// Utility function used to get the git version
// All the below replaces the following "gladed.androidgitversion" config:
// configure<com.gladed.androidgitversion.AndroidGitVersionExtension> {
//   codeFormat = "MNNPPBBBB"
//   format = "%tag%%-count%%-branch%%-dirty%"
//   prefix = "v"  // Only tags beginning with v are considered.
//   untrackedIsDirty = false
// }

fun ProviderFactory.git(vararg args: String): String {
	val result = exec {
		commandLine("git", *args)
		isIgnoreExitValue = true
	}
	return result.standardOutput.asText.get().trim()
}

val gitTag = providers.git("describe", "--tags", "--match", "v*", "--abbrev=0")
	.ifEmpty { "v0.0.0" }
val commitsSinceTag = providers.git(
	"rev-list", "$gitTag..HEAD", "--count"
).toIntOrNull() ?: 0
val branchName = providers.git(
	"rev-parse", "--abbrev-ref", "HEAD"
).replace("/", "-")
val isDirty = providers.git("status", "--porcelain")
	.lineSequence()
	.any { it.isNotBlank() && !it.startsWith("??") } // untrackedIsDirty = false

val versionParts = gitTag
	.removePrefix("v")
	.split(".")
	.mapNotNull { it.toIntOrNull() }

// Ensure we have at least 3 components by padding with 0
val major = versionParts.getOrElse(0) { 0 }
val minor = versionParts.getOrElse(1) { 0 }
val patch = versionParts.getOrElse(2) { 0 }

val gitVersionName = if (commitsSinceTag == 0) gitTag else buildString {
	append(gitTag)
	append("-")
	append(commitsSinceTag)
	append("-")
	append(branchName)
	if (isDirty) append("-dirty")
}

val gitVersionCode =
	major * 100_000_000 +
		minor * 1000_000 +
		patch * 10_000 +
		commitsSinceTag

dependencies {
	// 1.6.1 is the newest version we can use that won't complain about minSdk version,
	// and also doesn't collide kotlin versions with com.gladed.androidgitversion.
	// Will replace with a different plugin soon.
	implementation("androidx.appcompat:appcompat:1.7.1")
	implementation("androidx.documentfile:documentfile:1.1.0")
	implementation("com.google.protobuf:protobuf-javalite:4.33.5")

	// Reader fork: what the reading layer's screens are made with, and its JVM tests
	// (Robolectric supplies SQLite and JsonReader).
	implementation("com.google.android.material:material:1.12.0")
	implementation("androidx.preference:preference:1.2.1")
	implementation("androidx.recyclerview:recyclerview:1.3.2")
	implementation("androidx.core:core:1.13.1")
	testImplementation("junit:junit:4.13.2")
	testImplementation("org.json:json:20240303")
	testImplementation("org.robolectric:robolectric:4.14.1")
	testImplementation("androidx.test:core:1.6.1")
}

protobuf {
	protoc {
		artifact = "com.google.protobuf:protoc:3.25.3"
	}

	generateProtoTasks {
		all().forEach { task ->
			task.builtins {
				register("java") {
					option("lite")
				}
			}
		}
	}
}

android {
	flavorDimensions.add("variant")
	namespace = "org.ppsspp.ppsspp"
	signingConfigs {
		getByName("debug") {
			storeFile = file("debug.keystore")
		}
		create("optimized") {
			storeFile = file("debug.keystore")
		}
		if (project.hasProperty("RELEASE_STORE_FILE")) {
			create("release") {
				storeFile = file(project.property("RELEASE_STORE_FILE") as String)
				storePassword = project.property("RELEASE_STORE_PASSWORD") as String
				keyAlias = project.property("RELEASE_KEY_ALIAS") as String
				keyPassword = project.property("RELEASE_KEY_PASSWORD") as String
			}
		}
	}

	compileSdk = 36
	// Reader fork: another NDK can be named (-PREADER_NDK=...), to use one that is installed.
	ndkVersion = (project.findProperty("READER_NDK") as String?) ?: "29.0.14206865"

	compileOptions {
		sourceCompatibility = JavaVersion.VERSION_11
		targetCompatibility = JavaVersion.VERSION_11
	}

	lint {
		baseline = file("lint-baseline.xml")
	}

	// Reader fork: JVM tests of the reading layer see the app's resources.
	testOptions {
		unitTests {
			isIncludeAndroidResources = true
		}
	}

	defaultConfig {
		// Reader fork: false in a published build (-PreaderSaveSync=false): no save sync, and
		// no statistics sent to its service.
		buildConfigField("boolean", "READER_SAVE_SYNC",
			(providers.gradleProperty("readerSaveSync").orNull != "false").toString())
		applicationId = "org.ppsspp.ppsspp"
		// Access the git version info via the extension
		if (gitVersionName != "unknown") {
			println("INFO: Overriding Android Version Name, Code: $gitVersionName $gitVersionCode")
			versionName = gitVersionName
			versionCode = gitVersionCode
		} else {
			println("(not using these:) Android Version Name, Code: $gitVersionName $gitVersionCode")
		}
		file("versionname.txt").writeText(gitVersionName)
		file("versioncode.txt").writeText(gitVersionCode.toString())

		minSdk = 21
		targetSdk = 36
		if (project.hasProperty("ANDROID_VERSION_CODE") && project.hasProperty("ANDROID_VERSION_NAME")) {
			versionCode = (project.property("ANDROID_VERSION_CODE") as String).toInt()
			versionName = project.property("ANDROID_VERSION_NAME") as String
		}
	}
	buildTypes {
		getByName("debug") {
			isMinifyEnabled = false
			isJniDebuggable = true
			signingConfig = signingConfigs.getByName("debug")
		}
		create("optimized") {
			isMinifyEnabled = false
			isJniDebuggable = true
			signingConfig = signingConfigs.getByName("debug")
		}
		getByName("release") {
			isMinifyEnabled = false
			if (project.hasProperty("RELEASE_STORE_FILE")) {
				signingConfig = signingConfigs.getByName("release")
			} else {
				println("WARNING: RELEASE_STORE_FILE is missing. Release builds will be unusable.")
			}
		}
	}
	externalNativeBuild {
		cmake {
			path = file("../CMakeLists.txt")
		}
	}
	packaging {
		jniLibs.useLegacyPackaging = true
	}
	// Reader fork: a published build (-PreaderBundleScripts=false) comes with no game scripts;
	// they are installed from the published list (Game scripts). The patterns before the last
	// are aapt's own defaults.
	if (providers.gradleProperty("readerBundleScripts").orNull == "false") {
		androidResources {
			ignoreAssetsPattern = "!.svn:!.git:!.ds_store:!*.scc:.*:!CVS:!thumbs.db:!picasa.ini:!*~:!<dir>profiles"
		}
	}
	sourceSets {
		getByName("main") {
			manifest.srcFile("AndroidManifest.xml")
			res.directories.add("res")
			java.directories.add("src")
			kotlin.directories.add("src")  // Reader fork: the reading layer is Kotlin
			aidl.directories.add("src")
			resources.directories.add("src")
			assets.directories.add("../assets")
		}
		create("normal") {
			res.directories.add("normal/res")
		}
		create("gold") {
			res.directories.add("gold/res")
		}
		create("vr") {
			res.directories.add("normal/res")
			manifest.srcFile("VRManifest.xml")
		}
		create("legacy") {
			res.directories.add("legacy/res")
		}
		// Reader fork: the JVM tests of the reading layer.
		getByName("test") {
			java.directories.add("test")
			kotlin.directories.add("test")
			resources.directories.add("test-resources")
		}
		// Reader fork: the reader app's own name and its reading layer's resources.
		create("reader") {
			res.directories.add("reader/res")
			// The launcher icon. A set in private/icon, which is not in Git, comes before
			// the committed one: docs/reader/icon.md.
			// A published build (-PreaderPrivateIcon=false) always takes the committed one.
			val own = "../private/icon/android/res"
			val takeOwn = providers.gradleProperty("readerPrivateIcon").orNull != "false" && file(own).exists()
			res.directories.add(if (takeOwn) own else "../reader/icon/android/res")
		}
	}
	productFlavors {
		create("normal") {
			applicationId = "org.ppsspp.ppsspp"
			dimension = "variant"
			externalNativeBuild {
				cmake {
					// **FIXED**: Using simple "listOf"
					arguments.addAll(listOf(
						"-DANDROID=true",
						"-DANDROID_PLATFORM=android-21",
						"-DANDROID_TOOLCHAIN=clang",
						"-DANDROID_CPP_FEATURES=",
						"-DANDROID_STL=c++_shared"
					))
				}
			}
			ndk {
				abiFilters.addAll(listOf("armeabi-v7a", "arm64-v8a", "x86_64"))
				// debugSymbolLevel = "FULL"  // These don't actually help much in the Google Play crash report. We do still have symbols locally.
			}
		}
		// Reader fork: the app this fork is for. Its own application ID, so that it installs
		// beside PPSSPP instead of replacing it; the Java namespace stays org.ppsspp.ppsspp.
		create("reader") {
			applicationId = "io.github.sommepo.ppssppreader"
			dimension = "variant"
			externalNativeBuild {
				cmake {
					arguments.addAll(listOf(
						"-DANDROID=true",
						"-DANDROID_PLATFORM=android-21",
						"-DANDROID_TOOLCHAIN=clang",
						"-DANDROID_CPP_FEATURES=",
						"-DANDROID_STL=c++_shared"
					))
					(project.findProperty("READER_CMAKE_ARGS") as String?)?.let {
						arguments.addAll(it.split(" ").filter { arg -> arg.isNotBlank() })
					}
					// A published build (-PreaderSaveSync=false) has no save sync.
					if (providers.gradleProperty("readerSaveSync").orNull == "false") {
						arguments.add("-DREADER_SAVE_SYNC=OFF")
					}
				}
			}
			ndk {
				abiFilters.addAll(
					((project.findProperty("READER_ABIS") as String?) ?: "arm64-v8a").split(",")
				)
			}
		}
		create("gold") {
			applicationId = "org.ppsspp.ppssppgold"
			dimension = "variant"
			externalNativeBuild {
				cmake {
					arguments.addAll(listOf(
						"-DANDROID=true",
						"-DANDROID_PLATFORM=android-21",
						"-DANDROID_TOOLCHAIN=clang",
						"-DANDROID_CPP_FEATURES=",
						"-DANDROID_STL=c++_shared",
						"-DGOLD=TRUE"
					))
				}
			}
			ndk {
				abiFilters.addAll(listOf("armeabi-v7a", "arm64-v8a", "x86_64"))
				// debugSymbolLevel = "FULL"
			}
		}
		create("legacy") {
			applicationId = "org.ppsspp.ppsspplegacy"
			dimension = "variant"
			targetSdk = 29  // To avoid scoped storage, which is the point of the legacy APK
			externalNativeBuild {
				cmake {
					arguments.addAll(listOf(
						"-DANDROID=true",
						"-DANDROID_PLATFORM=android-21",
						"-DANDROID_TOOLCHAIN=clang",
						"-DANDROID_CPP_FEATURES=",
						"-DANDROID_STL=c++_shared",
						"-DANDROID_LEGACY=TRUE"
					))
				}
			}
			ndk {
				abiFilters.addAll(listOf("armeabi-v7a", "arm64-v8a"))
				// debugSymbolLevel = "FULL"
			}
		}
		create("vr") {
			applicationId = "org.ppsspp.ppssppvr"
			dimension = "variant"
			targetSdk = 29  // To avoid scoped storage, which doesn't work properly on Oculus
			externalNativeBuild {
				cmake {
					arguments.addAll(listOf(
						"-DANDROID=true",
						"-DANDROID_PLATFORM=android-21",
						"-DANDROID_TOOLCHAIN=clang",
						"-DANDROID_CPP_FEATURES=",
						"-DANDROID_STL=c++_shared",
						"-DOPENXR=TRUE",
						"-DANDROID_LEGACY=TRUE"
					))
				}
			}
			ndk {
				abiFilters.addAll(listOf("arm64-v8a"))
			}
		}
	}
	buildFeatures {
		aidl = true
		buildConfig = true
	}
}
androidComponents {
	beforeVariants(selector().all()) { variantBuilder ->
		// **FIXED**: Using simple "setOf"
		val enabledVariants = setOf(
			"normalDebug", "normalOptimized", "normalRelease",
			"goldDebug", "goldRelease",
			"vrDebug", "vrOptimized", "vrRelease",
			"legacyDebug", "legacyOptimized", "legacyRelease",
			"readerDebug", "readerOptimized", "readerRelease"  // Reader fork
		)
		variantBuilder.enable = variantBuilder.name in enabledVariants
	}
}

/*
afterEvaluate {
	android.sourceSets.getByName("main").assets.srcDirs.forEach {
		println(it)
	}
}*/

// Use the below to get a lot of deprecation warnings.
// Not really useful to fix most of them though, as the old paths are often needed to support
// old Android versions.
/*
tasks.withType<JavaCompile> {
	options.compilerArgs.addAll(listOf("-Xlint:deprecation"))
}
*/
