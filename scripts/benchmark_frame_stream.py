from __future__ import annotations

import argparse
import statistics
import sys
import time
from pathlib import Path

from PySide6.QtCore import QTimer
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication, QMainWindow


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

from native_webview_widget import NativeWebView  # noqa: E402


HTML = """
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<style>
html, body {
  width: 100%;
  height: 100%;
  margin: 0;
  overflow: hidden;
  background:
    radial-gradient(circle at var(--x, 20%) var(--y, 40%), #ffcc00 0 12%, transparent 25%),
    linear-gradient(120deg, #07111f, #163a5f 45%, #2b140d);
}
.card {
  position: absolute;
  left: 50%;
  top: 50%;
  width: 44vmin;
  height: 28vmin;
  transform: translate(-50%, -50%) rotate(var(--r, 0deg));
  border-radius: 28px;
  background: rgba(255,255,255,.12);
  border: 1px solid rgba(255,255,255,.35);
  box-shadow: 0 24px 80px rgba(0,0,0,.38);
  backdrop-filter: blur(10px);
}
</style>
</head>
<body>
<div class="card"></div>
<script>
const root = document.documentElement;
let t0 = performance.now();
function tick(t) {
  const s = (t - t0) / 1000;
  root.style.setProperty('--x', `${50 + Math.sin(s * 2.7) * 34}%`);
  root.style.setProperty('--y', `${50 + Math.cos(s * 2.1) * 30}%`);
  root.style.setProperty('--r', `${Math.sin(s * 3.0) * 18}deg`);
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);
</script>
</body>
</html>
""".strip()


class Benchmark:
    def __init__(self, every_values: list[int], seconds: float, width: int, height: int) -> None:
        self.every_values = every_values
        self.seconds = seconds
        self.results: list[tuple[int, int, float, float, float, str]] = []
        self.current_every = 0
        self.frame_times: list[float] = []
        self.bytes_total = 0
        self.frame_size = ""
        self.started_at = 0.0

        self.window = QMainWindow()
        self.window.resize(width, height)
        self.view = NativeWebView(session_id="frame-stream-benchmark")
        self.window.setCentralWidget(self.view)

        self.view.ready.connect(self._on_ready)
        self.view.frameStreamFrame.connect(self._on_frame)
        self.view.frameStreamFailed.connect(lambda error: print(f"stream failed: {error}", flush=True))

    def start(self) -> None:
        self.window.show()

    def _on_ready(self) -> None:
        self.view.set_html(HTML, "https://benchmark.local/")
        QTimer.singleShot(1200, self._run_next)

    def _run_next(self) -> None:
        if self.current_every:
            self.view.stop_frame_stream()
            elapsed = max(0.001, time.perf_counter() - self.started_at)
            frames = len(self.frame_times)
            fps = frames / elapsed
            avg_kb = (self.bytes_total / frames / 1024) if frames else 0.0
            intervals = [
                (b - a) * 1000
                for a, b in zip(self.frame_times, self.frame_times[1:])
            ]
            avg_interval = statistics.mean(intervals) if intervals else 0.0
            self.results.append((self.current_every, frames, fps, avg_kb, avg_interval, self.frame_size))
            print(
                f"everyNthFrame={self.current_every}: "
                f"{frames} frames in {elapsed:.2f}s = {fps:.2f} fps, "
                f"avg {avg_kb:.1f} KiB/frame, "
                f"size {self.frame_size or 'unknown'}, "
                f"avg interval {avg_interval:.1f} ms",
                flush=True,
            )

        if not self.every_values:
            print("\nSummary:", flush=True)
            for every, frames, fps, avg_kb, avg_interval, frame_size in self.results:
                print(
                    f"  everyNthFrame={every}: {fps:.2f} fps "
                    f"({frames} frames, {avg_kb:.1f} KiB/frame, "
                    f"{frame_size or 'unknown'}, {avg_interval:.1f} ms avg interval)",
                    flush=True,
                )
            QApplication.instance().quit()
            return

        self.current_every = self.every_values.pop(0)
        self.frame_times = []
        self.bytes_total = 0
        self.frame_size = ""
        self.started_at = time.perf_counter()
        ok = self.view.start_frame_stream(
            quality=75,
            max_width=self.view.width(),
            max_height=self.view.height(),
            every_nth_frame=self.current_every,
        )
        if not ok:
            raise RuntimeError("start_frame_stream returned False")
        print(f"running everyNthFrame={self.current_every} for {self.seconds:.1f}s", flush=True)
        QTimer.singleShot(int(self.seconds * 1000), self._run_next)

    def _on_frame(self, data: bytes) -> None:
        self.frame_times.append(time.perf_counter())
        self.bytes_total += len(data)
        if not self.frame_size:
            image = QImage()
            if image.loadFromData(data) and not image.isNull():
                self.frame_size = f"{image.width()}x{image.height()}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--every", type=int, nargs="+", default=[1, 2])
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    args = parser.parse_args()

    app = QApplication(sys.argv)
    bench = Benchmark(args.every, args.seconds, args.width, args.height)
    bench.start()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
