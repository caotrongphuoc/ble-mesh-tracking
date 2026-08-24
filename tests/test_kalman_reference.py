"""Golden behavior for the adaptive-R Kalman filter used in
apps/scanner/components/bmt_tag_table/bmt_tag_table.c.

Locks down: constant-signal convergence, outlier resistance from adaptive R,
and the K clamp behavior. Constants must match the C source below.
"""
import pytest


# Constants - MUST match bmt_tag_table.c
Q_FIXED = 0.1
R_ALPHA = 0.1
R_MIN = 1.0
R_MAX = 20.0
R_SEED = 2.0


class Kalman:
    def __init__(self, initial_rssi: float):
        self.q = Q_FIXED
        self.r = R_SEED
        self.r_var = R_SEED
        self.x = float(initial_rssi)
        self.p = 1.0
        self.k = 0.0

    def update(self, rssi: float) -> float:
        innovation = rssi - self.x
        self.r_var = (1.0 - R_ALPHA) * self.r_var + R_ALPHA * (innovation * innovation)
        self.r = max(R_MIN, min(R_MAX, self.r_var))
        self.p = self.p + self.q
        self.k = self.p / (self.p + self.r)
        self.x = self.x + self.k * innovation
        self.p = (1.0 - self.k) * self.p
        return self.x


def test_converges_on_constant_signal():
    """Feed the same value 30 times - filter should sit on it."""
    kf = Kalman(initial_rssi=-70.0)
    for _ in range(30):
        out = kf.update(-70.0)
    assert out == pytest.approx(-70.0, abs=0.01)


def test_tracks_step_change_eventually():
    """After a large step, adaptive R spikes and K shrinks -> slow tracking.
    Filter must still converge to the new level within ~100 samples (~50 s
    at the tag's 500 ms beacon rate). Wider tolerance because R stays
    inflated for a while after the step."""
    kf = Kalman(initial_rssi=-70.0)
    for _ in range(50):
        kf.update(-70.0)   # settle
    for _ in range(100):
        out = kf.update(-50.0)  # step up
    assert out == pytest.approx(-50.0, abs=3.0)


def test_outlier_does_not_swing_filter():
    """One extreme sample must not yank the filter to it. Adaptive R
    grows on large innovation -> K shrinks -> outlier gets attenuated."""
    kf = Kalman(initial_rssi=-70.0)
    for _ in range(50):
        kf.update(-70.0)   # very stable at -70
    before = kf.x
    kf.update(0.0)         # absurd outlier
    after = kf.x
    # Movement should be less than half the delta (adaptive R kicks in).
    assert abs(after - before) < 0.5 * abs(0.0 - before)


def test_kalman_gain_stays_in_valid_range():
    """K must stay strictly inside (0, 1) - the clamps on r prevent
    degenerate cases (K=0 = frozen, K=1 = no filtering)."""
    kf = Kalman(initial_rssi=-70.0)
    for rssi in [-70, -75, -60, -80, -50, -70, -90, -70]:
        kf.update(rssi)
        assert 0.0 < kf.k < 1.0, f"K={kf.k} out of range after rssi={rssi}"


def test_r_stays_clamped():
    """r_var can compute anything but the clamped r must stay in [R_MIN, R_MAX]."""
    kf = Kalman(initial_rssi=-70.0)
    # Inject a huge innovation to blow up r_var
    for _ in range(5):
        kf.update(-70.0)
    kf.update(50.0)   # innovation^2 = 14400 -> r_var spikes
    assert R_MIN <= kf.r <= R_MAX
