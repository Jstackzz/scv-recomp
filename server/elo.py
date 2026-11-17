"""Elo rating arithmetic (zero-sum, integer ratings) and the ranked titles the ratings hold."""

from __future__ import annotations

from typing import Any

INITIAL = 475
K_FACTOR = 32

# The ranked titles by Elo, lowest to highest: each holds from its Elo up to the next one's, the lowest below 575.
# Each has its badge, which the launcher draws by title (the title in lower case, dashes for spaces: soul-warrior.png).
# A new player starts at INITIAL, a Novice, one step below Apprentice. The steps up: 100 Elo a rank up to Soul Warrior,
# 120 a rank from Soul Warrior on.
RANKS: tuple[tuple[int | None, str], ...] = (
    (None, "Novice"),
    (575, "Apprentice"),
    (675, "Squire"),
    (775, "Wanderer"),
    (875, "Duelist"),
    (975, "Knight"),
    (1075, "Vanguard"),
    (1175, "Blademaster"),
    (1275, "Champion"),
    (1375, "Conqueror"),
    (1475, "Hero"),
    (1575, "Legend"),
    (1675, "Soul Warrior"),
    (1795, "Soul Master"),
    (1915, "Spirit Sword"),
    (2035, "Eternal Soul"),
)


def title(rating: int) -> str:
    """The ranked title a rating holds."""
    for floor, name in reversed(RANKS):
        if floor is None or rating >= floor:
            return name
    raise AssertionError("RANKS starts with a title for every rating")


def ladder() -> list[dict[str, Any]]:
    """The titles in order with the Elo each starts at (None: the lowest, below the next one's)."""
    return [{"title": name, "from": floor} for floor, name in RANKS]


def expected(rating_a: float, rating_b: float) -> float:
    """Probability that A beats B."""
    return 1.0 / (1.0 + 10.0 ** ((rating_b - rating_a) / 400.0))


def delta(rating_a: int, rating_b: int, score_a: float, k: int = K_FACTOR) -> int:
    """Rating change of A (B changes by the negative), rounded half away from zero."""
    raw = k * (score_a - expected(rating_a, rating_b))
    return int(raw + 0.5) if raw >= 0 else -int(-raw + 0.5)


def update(rating_a: int, rating_b: int, score_a: float, k: int = K_FACTOR) -> tuple[int, int]:
    """New (A, B) ratings after a game where A scored `score_a` (1 win, 0 loss)."""
    d = delta(rating_a, rating_b, score_a, k)
    return rating_a + d, rating_b - d


# A ranked set's winner gains this many times the Elo change, the loser loses the change itself (since 2026-11-12, the
# user: "double the amount of elo gained for each set win"), so ratings climb faster than they fall.
SET_WIN_GAIN = 2


def set_result(winner: int, loser: int, k: int = K_FACTOR) -> tuple[int, int]:
    """New (winner, loser) ratings after a ranked set: the winner gains SET_WIN_GAIN times the plain change."""
    d = delta(winner, loser, 1.0, k)
    return winner + SET_WIN_GAIN * d, loser - d
