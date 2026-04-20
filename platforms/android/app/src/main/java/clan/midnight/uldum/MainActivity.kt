package clan.midnight.uldum

import com.google.androidgamesdk.GameActivity

/**
 * Thin GameActivity subclass — all app logic lives in the native library
 * (`libuldum_game.so`, loaded automatically via the AndroidManifest's
 * `android.app.lib_name` metadata).
 */
class MainActivity : GameActivity()
