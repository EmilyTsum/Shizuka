# SPDX-License-Identifier: GPL-2.0-only
"""Exercise presentation policy and a source guard; runtime theme QA is separate."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from shizuka.java import java_tools  # noqa: E402


class LatencyApplyUi(unittest.TestCase):
    def test_oneui_apply_button_source_avoids_material_theme_requirement(self):
        # This protects the known bad constructor choice; it does not execute
        # Android theme enforcement. The module's real tuning page also needs QA.
        import xml.etree.ElementTree as xml

        app = ROOT / "tools/android-app"
        styles = xml.parse(app / "res/values/styles.xml")
        theme = styles.find("./style[@name='AppTheme']")
        self.assertIsNotNone(theme)
        self.assertEqual(theme.attrib["parent"], "OneUITheme")
        source = (app / "src/dev/shizuka/settings/MainActivity.java").read_text()
        tuning = source.split("private void tuning(PreferenceScreen screen)", 1)[1].split(
            "private void dsp(PreferenceScreen screen)", 1
        )[0]
        self.assertNotIn("MaterialButton", source)
        self.assertIn("new AppCompatButton(requireContext())", tuning)

    def test_foreground_and_render_policies_are_connected(self):
        # Source bindings complement the executable policy tests below; they do
        # not replace the module's actual foreground UI verification.
        source = (ROOT / "tools/android-app/src/dev/shizuka/settings/MainActivity.java").read_text()
        self.assertIn("ui.postDelayed(this, DspDisplayState.pollInterval(page))", source)
        self.assertIn("ui.postDelayed(poll, DspDisplayState.pollInterval(page))", source)
        self.assertIn('"frame_bitrate", dspBitrateKey(dsp)', source)
        cache = source.split('if (page.equals("tuning")) {', 1)[1]
        self.assertLess(
            cache.index("scheduleDelayExpiry(operation)"),
            cache.index("visible.equals(renderedTuning)"),
        )

    def test_normal_launcher_and_fixed_bootstrap_binding(self):
        import xml.etree.ElementTree as xml

        app = ROOT / "tools/android-app"
        manifest = xml.parse(app / "AndroidManifest.xml")
        ns = "{http://schemas.android.com/apk/res/android}"
        activity = manifest.find("./application/activity")
        self.assertEqual(activity.attrib[ns + "exported"], "true")
        self.assertEqual(
            activity.find("./intent-filter/action").attrib[ns + "name"],
            "android.intent.action.MAIN",
        )
        self.assertEqual(
            activity.find("./intent-filter/category").attrib[ns + "name"],
            "android.intent.category.LAUNCHER",
        )
        self.assertEqual(manifest.findall("./application/receiver"), [])
        root_api = (app / "src/dev/shizuka/settings/RootApi.java").read_text()
        self.assertIn("new ProcessBuilder(ModuleSession.command(uid))", root_api)
        self.assertIn("android.os.Process.myUid()", root_api)
        self.assertIn("process.waitFor(ModuleSession.TIMEOUT_SECONDS", root_api)
        source = (app / "src/dev/shizuka/settings/MainActivity.java").read_text()
        self.assertIn("connectionWorker.execute", source)
        self.assertIn("connectionGeneration != api.generation()", source)
        poll = source.split("private final Runnable poll", 1)[1].split(
            "@Override public void onCreate", 1
        )[0]
        self.assertNotIn(
            "connectModule", poll
        )  # Failed permission requests require an explicit retry.

    def test_dsp_deadline_chain_is_rearmed_before_the_unchanged_view_return(self):
        # The executable deadline chain below supplies no new API receipt.
        # These bindings ensure MainActivity uses it before its actual cache return.
        source = (ROOT / "tools/android-app/src/dev/shizuka/settings/MainActivity.java").read_text()
        render = source.split("private void render() {", 1)[1].split(
            "private void scheduleDspExpiry()", 1
        )[0]
        self.assertLess(
            render.index("scheduleDspExpiry()"),
            render.index("overview.equals(renderedDspOverview)"),
        )
        scheduler = source.split("private void scheduleDspExpiry()", 1)[1].split(
            "private List<Object> tuningDisplayKey", 1
        )[0]
        self.assertIn("ui.removeCallbacks(dspExpiry)", scheduler)
        self.assertIn("DspDisplayState.nextExpiryDelay", scheduler)
        self.assertIn('!"encoder_epoch".equals(field.optString("scope"))', scheduler)
        self.assertIn("if (delay > 0) ui.postDelayed(dspExpiry, delay)", scheduler)
        self.assertIn("private final Runnable dspExpiry = () -> { if (resumed) render(); }", source)
        pause = source.split("void onPause()", 1)[1].split("void onDestroy()", 1)[0]
        destroy = source.split("void onDestroy()", 1)[1].split("void onSaveInstanceState", 1)[0]
        self.assertIn("ui.removeCallbacks(dspExpiry)", pause)
        self.assertIn("ui.removeCallbacks(dspExpiry)", destroy)

    def test_operation_identity_freshness_and_english_states(self):
        java, compiler = java_tools()
        with tempfile.TemporaryDirectory(prefix="shizuka-apply-ui-") as directory:
            sources = [
                ROOT / "tools/android-app/src/dev/shizuka/settings/LatencyApplyState.java",
                ROOT / "tests/LatencyApplyStateTest.java",
                ROOT / "tools/android-app/src/dev/shizuka/settings/TuningDisplayState.java",
                ROOT / "tests/TuningDisplayStateTest.java",
                ROOT / "tools/android-app/src/dev/shizuka/settings/ObservationFreshness.java",
                ROOT / "tools/android-app/src/dev/shizuka/settings/DspDisplayState.java",
                ROOT / "tests/DspDisplayStateTest.java",
                ROOT / "tools/android-app/src/dev/shizuka/settings/ModuleSession.java",
                ROOT / "tests/ModuleSessionTest.java",
            ]
            subprocess.run(
                [*compiler, "--release", "8", "-d", directory, *map(str, sources)],
                check=True,
                capture_output=True,
                text=True,
                timeout=30,
            )
            result = subprocess.run(
                [java, "-cp", directory, "dev.shizuka.settings.LatencyApplyStateTest"],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertIn("checks passed", result.stdout)
            print(result.stdout.strip())
            projection = subprocess.run(
                [java, "-cp", directory, "dev.shizuka.settings.TuningDisplayStateTest"],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertIn("checks passed", projection.stdout)
            print(projection.stdout.strip())
            dsp = subprocess.run(
                [java, "-cp", directory, "dev.shizuka.settings.DspDisplayStateTest"],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertIn("checks passed", dsp.stdout)
            print(dsp.stdout.strip())
            session = subprocess.run(
                [java, "-cp", directory, "dev.shizuka.settings.ModuleSessionTest"],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertIn("checks passed", session.stdout)
            print(session.stdout.strip())


if __name__ == "__main__":
    unittest.main()
