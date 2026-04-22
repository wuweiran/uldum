package clan.midnight.uldum

import com.google.androidgamesdk.GameActivity

/**
 * Thin GameActivity subclass — all app logic lives in `libuldum.so`, loaded
 * automatically via the AndroidManifest's `android.app.lib_name` metadata.
 * Same .so name for both dev and game flavors; they produce separate APKs
 * with separate applicationIds.
 */
class MainActivity : GameActivity()
