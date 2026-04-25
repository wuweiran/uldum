package clan.midnight.uldum

import android.os.Bundle
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.google.androidgamesdk.GameActivity

/**
 * Thin GameActivity subclass — all app logic lives in `libuldum.so`, loaded
 * automatically via the AndroidManifest's `android.app.lib_name` metadata.
 * Same .so name for both dev and game flavors; they produce separate APKs
 * with separate applicationIds.
 *
 * Window setup:
 *   1. Go edge-to-edge so the framebuffer spans the whole display. Without
 *      this the framebuffer already excludes the nav bar on many devices,
 *      GameActivity reports zero insets for it, and the HUD has no way to
 *      know where to pull back from.
 *   2. Hide the status bar entirely (we're a landscape game — the
 *      notification strip only eats real estate).
 *   3. Leave the navigation bar visible; the HUD's safe-area plumbing
 *      keeps composites out of it via GameActivity_getWindowInsets.
 *   4. BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE so the user can still pull
 *      down notifications — the status bar slides in over the game
 *      briefly, then auto-hides.
 */
class MainActivity : GameActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        super.onCreate(savedInstanceState)

        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.hide(WindowInsetsCompat.Type.statusBars())
        controller.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    }

    // Android can restore the status bar when the window regains focus
    // (multi-window pop-ins, notifications, dialog dismissals). Re-hide
    // it every time focus comes back so the game stays in its intended
    // chrome-less state.
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            WindowInsetsControllerCompat(window, window.decorView)
                .hide(WindowInsetsCompat.Type.statusBars())
        }
    }
}
