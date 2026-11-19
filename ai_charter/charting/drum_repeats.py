"""Compassos que se repetem na música saem iguais na chart, como um charter faria.

As charts oficiais repetem muito: ~70% dos compassos têm um gêmeo idêntico na mesma música (o
refrão que volta, a levada do verso). A transcrição, golpe a golpe, erra um pouco diferente em cada
repetição. Aqui os compassos que soam parecidos (o mesmo desenho de peças e forças nas semicolcheias)
formam um grupo, e cada nota do grupo fica ou sai por maioria, igual em todos eles. De quebra, um
erro isolado numa das repetições é corrigido pelas outras.
"""

from __future__ import annotations

from dataclasses import replace
from typing import Callable, Hashable

import numpy as np

from ai_charter.charting.common import Candidate


def consolidate_repeats(cands: list[Candidate], bar_starts: np.ndarray, resolution: int,
                        similarity: float = 0.9, majority: float = 0.5) -> list[Candidate]:
    """`bar_starts`: tick do início de cada compasso. Só compassos com todas as notas na grade de
    semicolcheias entram nos grupos (tercinas e afins ficam como estão)."""
    return _consolidate(cands, bar_starts, resolution, similarity, majority, key=lambda c: c.piece,
                        rebuild=lambda members, tick, strength, piece: Candidate(tick, strength, piece=piece))


def consolidate_pitched(cands: list[Candidate], bar_starts: np.ndarray, resolution: int,
                        similarity: float = 0.8, majority: float = 0.5) -> list[Candidate]:
    """Guitarra, baixo e teclado: um riff que se repete (palhetadas abafadas, a levada do baixo) sai
    igual, como a bateria. Diferente dela, a altura detectada de cada nota oscila (a distorção faz um
    power chord de Mi parecer Mi, Si ou Dó de um compasso para outro), então os compassos se agrupam
    pelo RITMO e pelas CLASSES DE NOTA em jogo, não pela altura exata de cada ataque: o mesmo ritmo
    noutras notas (o refrão em Lá) é outro grupo. Em cada grupo, cada posição fica ou sai por maioria
    e leva a altura em que a maioria concorda, com o acorde, as vozes e a duração da nota mais forte
    nessa altura. A grade é de 1/12 de batida: semicolcheias e tercinas."""
    if len(bar_starts) < 2 or not cands:
        return cands
    step = resolution / 12
    bar_of = np.searchsorted(bar_starts, [c.tick for c in cands], side="right") - 1
    by_bar: dict[int, list[Candidate]] = {}
    for b, c in zip(bar_of, cands):
        by_bar.setdefault(int(b), []).append(c)
    slots = int(round(np.median(np.diff(bar_starts)) / step))
    rhythms, classes, bars, positions = [], [], [], {}
    for b, members in by_bar.items():
        if b < 0 or b + 1 >= len(bar_starts) or len(members) < 3:
            continue
        pos = [(c.tick - bar_starts[b]) / step for c in members]
        if any(abs(q - round(q)) > 1e-6 or not 0 <= round(q) < slots for q in pos):
            continue
        r, pc = np.zeros(slots), np.zeros(12)
        for q, c in zip(pos, members):
            r[int(round(q))] = 1.0
            pc[int(round(c.pitch)) % 12] += 1.0
        rhythms.append(r / np.linalg.norm(r))
        classes.append(pc / (np.linalg.norm(pc) + 1e-9))
        bars.append(b)
        positions[b] = [int(round(q)) for q in pos]
    if len(bars) < 2:
        return cands
    groups: list[list[int]] = []
    centers: list[tuple[np.ndarray, np.ndarray]] = []
    for i in range(len(bars)):            # cada compasso entra no grupo mais parecido (ou abre um)
        best, best_sim = -1, -1.0
        for j, (cr, cp) in enumerate(centers):
            sim = 0.5 * (cr @ rhythms[i]) / (np.linalg.norm(cr) + 1e-9) + 0.5 * (cp @ classes[i]) / (np.linalg.norm(cp) + 1e-9)
            if sim > best_sim:
                best, best_sim = j, sim
        if best >= 0 and best_sim >= similarity:
            groups[best].append(i)
            centers[best] = (centers[best][0] + rhythms[i], centers[best][1] + classes[i])
        else:
            groups.append([i])
            centers.append((rhythms[i].copy(), classes[i].copy()))
    replaced: dict[int, list[Candidate]] = {}
    for g in groups:
        if len(g) < 2:
            continue
        at: dict[int, list[Candidate]] = {}
        for i in g:
            for q, c in zip(positions[bars[i]], by_bar[bars[i]]):
                at.setdefault(q, []).append(c)
        chosen: dict[int, Candidate] = {}
        for q, members in at.items():
            if len(members) < majority * len(g):
                continue
            votes: dict[int, list[Candidate]] = {}
            for c in members:
                votes.setdefault(int(round(c.pitch)), []).append(c)
            pitch = max(votes, key=lambda p: (len(votes[p]), max(c.strength for c in votes[p])))
            chosen[q] = max(votes[pitch], key=lambda c: c.strength)
        for i in g:
            b = bars[i]
            replaced[b] = []
            for q, best in chosen.items():
                tick = int(round(bar_starts[b] + q * step))
                replaced[b].append(replace(best, tick=tick, end_tick=tick + max(0, best.end_tick - best.tick)))
    out = [c for b, c in zip(bar_of, cands) if int(b) not in replaced]
    for notes in replaced.values():
        out += notes
    return sorted(out, key=lambda c: c.tick)


def _consolidate(cands: list[Candidate], bar_starts: np.ndarray, resolution: int, similarity: float,
                 majority: float, key: Callable[[Candidate], Hashable],
                 rebuild: Callable[[list[Candidate], int, float, Hashable], Candidate]) -> list[Candidate]:
    if len(bar_starts) < 2 or not cands:
        return cands
    step = resolution / 4
    pieces = sorted({key(c) for c in cands})
    col = {p: i for i, p in enumerate(pieces)}
    bar_of = np.searchsorted(bar_starts, [c.tick for c in cands], side="right") - 1
    by_bar: dict[int, list[Candidate]] = {}
    for b, c in zip(bar_of, cands):
        by_bar.setdefault(int(b), []).append(c)
    slots = int(round((np.median(np.diff(bar_starts)) if len(bar_starts) > 1 else 4 * resolution) / step))
    vectors, bars = [], []
    for b, members in by_bar.items():
        if b < 0 or b + 1 >= len(bar_starts) or len(members) < 3:
            continue
        pos = [(c.tick - bar_starts[b]) / step for c in members]
        if any(abs(q - round(q)) > 1e-6 or not 0 <= round(q) < slots for q in pos):
            continue
        v = np.zeros((slots, len(pieces)))
        for q, c in zip(pos, members):
            v[int(round(q)), col[key(c)]] = max(v[int(round(q)), col[key(c)]], c.strength)
        vectors.append(v.ravel())
        bars.append(b)
    if len(bars) < 2:
        return cands
    V = np.array(vectors)
    V /= np.linalg.norm(V, axis=1, keepdims=True) + 1e-9
    groups: list[list[int]] = []
    centers: list[np.ndarray] = []
    for i in range(len(bars)):            # cada compasso entra no grupo mais parecido (ou abre um)
        if centers:
            sims = np.array([c @ V[i] / (np.linalg.norm(c) + 1e-9) for c in centers])
            j = int(sims.argmax())
            if sims[j] >= similarity:
                groups[j].append(i)
                centers[j] = centers[j] + V[i]
                continue
        groups.append([i])
        centers.append(V[i].copy())
    replaced: dict[int, list[Candidate]] = {}
    for g in groups:
        if len(g) < 2:
            continue
        notes: dict[tuple[int, Hashable], list[Candidate]] = {}
        for i in g:
            b = bars[i]
            for c in by_bar[b]:
                notes.setdefault((int(round((c.tick - bar_starts[b]) / step)), key(c)), []).append(c)
        keep = {k: v for k, v in notes.items() if len(v) >= majority * len(g)}
        for i in g:
            b = bars[i]
            replaced[b] = [rebuild(v, int(round(bar_starts[b] + q * step)), float(np.mean([c.strength for c in v])), p)
                           for (q, p), v in keep.items()]
    out = [c for b, c in zip(bar_of, cands) if int(b) not in replaced]
    for notes in replaced.values():
        out += notes
    return sorted(out, key=lambda c: (c.tick, repr(key(c))))
