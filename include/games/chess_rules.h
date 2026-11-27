#pragma once

#include <stdint.h>
#include <string.h>

// Chess rules and a small computer player, kept apart from the drawing.
// Castling, en passant, check, checkmate, stalemate, the 50-move rule and
// too-little-material draws are handled; a pawn always becomes a queen.
namespace chess {

enum : uint8_t { EMPTY, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
constexpr uint8_t WHITE = 0;  // starts at the bottom and moves up
constexpr uint8_t BLACK = 8;  // a black piece is its type plus this
// Castling still allowed.
constexpr uint8_t WHITE_SHORT = 1, WHITE_LONG = 2, BLACK_SHORT = 4,
                  BLACK_LONG = 8;
// Results.
constexpr uint8_t PLAYING = 0, WHITE_WINS = 1, BLACK_WINS = 2, STALEMATE = 3,
                  DRAW = 4;
constexpr uint16_t MAX_MOVES = 220;  // more than any position can have

struct Move {
  uint8_t from;
  uint8_t to;
};

struct State {
  uint8_t cells[64];  // row * 8 + column, row 0 at the top (black's side)
  uint8_t turn;       // WHITE or BLACK
  uint8_t castle;
  int8_t passant;  // the square a pawn just skipped over, or -1
  uint8_t quiet;   // half-moves without a capture or a pawn moving
  uint8_t king[2];  // where the white and black kings are
};

inline uint8_t typeOf(uint8_t piece) { return piece & 7; }
inline uint8_t sideOf(uint8_t piece) { return piece & 8; }
inline bool isSide(uint8_t piece, uint8_t side) {
  return piece != EMPTY && sideOf(piece) == side;
}

inline void reset(State& s) {
  static const uint8_t BACK[] = {ROOK, KNIGHT, BISHOP, QUEEN,
                                 KING, BISHOP, KNIGHT, ROOK};
  memset(s.cells, EMPTY, sizeof(s.cells));
  for (uint8_t c = 0; c < 8; ++c) {
    s.cells[c] = BACK[c] | BLACK;
    s.cells[8 + c] = PAWN | BLACK;
    s.cells[48 + c] = PAWN;
    s.cells[56 + c] = BACK[c];
  }
  s.turn = WHITE;
  s.castle = WHITE_SHORT | WHITE_LONG | BLACK_SHORT | BLACK_LONG;
  s.passant = -1;
  s.quiet = 0;
  s.king[0] = 60;
  s.king[1] = 4;
}

constexpr int8_t KNIGHT_STEPS[8][2] = {{-2, -1}, {-2, 1}, {-1, -2}, {-1, 2},
                                       {1, -2},  {1, 2},  {2, -1},  {2, 1}};
// The first four are straight, the last four diagonal.
constexpr int8_t RAYS[8][2] = {{-1, 0}, {1, 0},  {0, -1}, {0, 1},
                               {-1, -1}, {-1, 1}, {1, -1}, {1, 1}};

inline bool onBoard(int8_t row, int8_t col) {
  return row >= 0 && row < 8 && col >= 0 && col < 8;
}

// True if a piece of `by` attacks the square.
inline bool attacked(const State& s, uint8_t square, uint8_t by) {
  const int8_t row = square / 8, col = square % 8;
  // A pawn attacks from the row behind it, as seen from its own side.
  const int8_t pawnRow = row + (by == WHITE ? 1 : -1);
  for (int8_t dc = -1; dc <= 1; dc += 2) {
    if (onBoard(pawnRow, col + dc) &&
        s.cells[pawnRow * 8 + col + dc] == (PAWN | by)) {
      return true;
    }
  }
  for (const auto& step : KNIGHT_STEPS) {
    const int8_t r = row + step[0], c = col + step[1];
    if (onBoard(r, c) && s.cells[r * 8 + c] == (KNIGHT | by)) return true;
  }
  for (uint8_t ray = 0; ray < 8; ++ray) {
    int8_t r = row + RAYS[ray][0], c = col + RAYS[ray][1];
    for (uint8_t distance = 1; onBoard(r, c); ++distance) {
      const uint8_t piece = s.cells[r * 8 + c];
      if (piece != EMPTY) {
        if (sideOf(piece) == by) {
          const uint8_t type = typeOf(piece);
          if (type == QUEEN || type == (ray < 4 ? ROOK : BISHOP) ||
              (type == KING && distance == 1)) {
            return true;
          }
        }
        break;
      }
      r += RAYS[ray][0];
      c += RAYS[ray][1];
    }
  }
  return false;
}

inline bool inCheck(const State& s) {
  return attacked(s, s.king[s.turn >> 3], s.turn ^ BLACK);
}

inline void apply(State& s, const Move& m) {
  const uint8_t piece = s.cells[m.from];
  const uint8_t type = typeOf(piece);
  const uint8_t side = sideOf(piece);
  bool captured = s.cells[m.to] != EMPTY;
  s.cells[m.from] = EMPTY;
  s.cells[m.to] = piece;
  if (type == PAWN) {
    if (m.to == s.passant) {  // the pawn that skipped past is taken
      s.cells[m.to + (side == WHITE ? 8 : -8)] = EMPTY;
      captured = true;
    }
    if (m.to / 8 == (side == WHITE ? 0 : 7)) s.cells[m.to] = QUEEN | side;
  }
  if (type == KING) {
    s.king[side >> 3] = m.to;
    if (m.to == m.from + 2) {  // castling brings the rook across
      s.cells[m.from + 1] = s.cells[m.from + 3];
      s.cells[m.from + 3] = EMPTY;
    } else if (m.to + 2 == m.from) {
      s.cells[m.from - 1] = s.cells[m.from - 4];
      s.cells[m.from - 4] = EMPTY;
    }
    s.castle &= side == WHITE ? ~(WHITE_SHORT | WHITE_LONG)
                              : ~(BLACK_SHORT | BLACK_LONG);
  }
  // A rook that moves or is taken can no longer castle.
  for (const uint8_t square : {m.from, m.to}) {
    if (square == 63) s.castle &= ~WHITE_SHORT;
    if (square == 56) s.castle &= ~WHITE_LONG;
    if (square == 7) s.castle &= ~BLACK_SHORT;
    if (square == 0) s.castle &= ~BLACK_LONG;
  }
  s.passant = type == PAWN && (m.to == m.from + 16 || m.to + 16 == m.from)
                  ? (m.from + m.to) / 2
                  : -1;
  if (captured || type == PAWN) {
    s.quiet = 0;
  } else if (s.quiet < 100) {
    ++s.quiet;
  }
  s.turn ^= BLACK;
}

// Every move that follows how the pieces step, before asking whether it
// leaves the king in check. `loud` keeps only captures and promotions.
inline uint16_t pseudoMoves(const State& s, Move* out, bool loud) {
  uint16_t count = 0;
  const uint8_t side = s.turn;
  const uint8_t enemy = side ^ BLACK;
  const auto add = [&](uint8_t from, int8_t r, int8_t c) {
    const uint8_t to = r * 8 + c;
    if (isSide(s.cells[to], side)) return;
    if (loud && s.cells[to] == EMPTY) return;
    out[count++] = {from, to};
  };
  for (uint8_t from = 0; from < 64; ++from) {
    const uint8_t piece = s.cells[from];
    if (!isSide(piece, side)) continue;
    const uint8_t type = typeOf(piece);
    const int8_t row = from / 8, col = from % 8;
    if (type == PAWN) {
      const int8_t dir = side == WHITE ? -1 : 1;
      const int8_t r = row + dir;
      if (r < 0 || r > 7) continue;
      const bool promotes = r == 0 || r == 7;
      if (s.cells[r * 8 + col] == EMPTY && (!loud || promotes)) {
        out[count++] = {from, static_cast<uint8_t>(r * 8 + col)};
        if (row == (side == WHITE ? 6 : 1) &&
            s.cells[(r + dir) * 8 + col] == EMPTY) {
          out[count++] = {from, static_cast<uint8_t>((r + dir) * 8 + col)};
        }
      }
      for (int8_t dc = -1; dc <= 1; dc += 2) {
        if (!onBoard(r, col + dc)) continue;
        const uint8_t to = r * 8 + col + dc;
        if (isSide(s.cells[to], enemy) || to == s.passant) {
          out[count++] = {from, to};
        }
      }
    } else if (type == KNIGHT) {
      for (const auto& step : KNIGHT_STEPS) {
        if (onBoard(row + step[0], col + step[1])) {
          add(from, row + step[0], col + step[1]);
        }
      }
    } else {
      const uint8_t first = type == BISHOP ? 4 : 0;
      const uint8_t last = type == ROOK ? 4 : 8;
      for (uint8_t ray = first; ray < last; ++ray) {
        int8_t r = row + RAYS[ray][0], c = col + RAYS[ray][1];
        while (onBoard(r, c)) {
          add(from, r, c);
          if (type == KING || s.cells[r * 8 + c] != EMPTY) break;
          r += RAYS[ray][0];
          c += RAYS[ray][1];
        }
      }
    }
    // Castling: the squares between are empty and the king does not start on,
    // pass over or land on an attacked square.
    if (type != KING || loud) continue;
    const uint8_t home = side == WHITE ? 60 : 4;
    if (from != home || attacked(s, home, enemy)) continue;
    if ((s.castle & (side == WHITE ? WHITE_SHORT : BLACK_SHORT)) &&
        s.cells[home + 3] == (ROOK | side) && s.cells[home + 1] == EMPTY &&
        s.cells[home + 2] == EMPTY && !attacked(s, home + 1, enemy) &&
        !attacked(s, home + 2, enemy)) {
      out[count++] = {home, static_cast<uint8_t>(home + 2)};
    }
    if ((s.castle & (side == WHITE ? WHITE_LONG : BLACK_LONG)) &&
        s.cells[home - 4] == (ROOK | side) && s.cells[home - 1] == EMPTY &&
        s.cells[home - 2] == EMPTY && s.cells[home - 3] == EMPTY &&
        !attacked(s, home - 1, enemy) && !attacked(s, home - 2, enemy)) {
      out[count++] = {home, static_cast<uint8_t>(home - 2)};
    }
  }
  return count;
}

// Plays `m` into `next`; false if it would leave the mover's king in check.
inline bool tryMove(const State& s, const Move& m, State& next) {
  next = s;
  apply(next, m);
  return !attacked(next, next.king[s.turn >> 3], next.turn);
}

// Every move the side to move may make. `out` holds MAX_MOVES.
inline uint16_t legalMoves(const State& s, Move* out) {
  const uint16_t count = pseudoMoves(s, out, false);
  uint16_t kept = 0;
  State next;
  for (uint16_t i = 0; i < count; ++i) {
    if (tryMove(s, out[i], next)) out[kept++] = out[i];
  }
  return kept;
}

// Neither side has enough left to give checkmate.
inline bool bareBoard(const State& s) {
  uint8_t minor = 0;
  for (uint8_t i = 0; i < 64; ++i) {
    const uint8_t type = typeOf(s.cells[i]);
    if (type == PAWN || type == ROOK || type == QUEEN) return false;
    if (type == KNIGHT || type == BISHOP) ++minor;
  }
  return minor <= 1;
}

inline uint8_t result(const State& s) {
  Move moves[MAX_MOVES];
  if (legalMoves(s, moves) == 0) {
    if (!inCheck(s)) return STALEMATE;
    return s.turn == WHITE ? BLACK_WINS : WHITE_WINS;
  }
  return s.quiet >= 100 || bareBoard(s) ? DRAW : PLAYING;
}

// A number for the position, to notice one coming round a third time.
inline uint32_t fingerprint(const State& s) {
  uint32_t hash = 2166136261u;
  const auto mix = [&](uint8_t byte) { hash = (hash ^ byte) * 16777619u; };
  for (uint8_t i = 0; i < 64; ++i) mix(s.cells[i]);
  mix(s.turn);
  mix(s.castle);
  mix(static_cast<uint8_t>(s.passant));
  return hash;
}

// ---- The computer player ----------------------------------------------------

constexpr int16_t VALUES[] = {0, 100, 310, 320, 500, 900, 0};
constexpr int INFINITE = 30000;

// How good the board looks to the side to move.
inline int evaluate(const State& s) {
  int score[2] = {0, 0};
  int material[2] = {0, 0};
  for (uint8_t i = 0; i < 64; ++i) {
    const uint8_t piece = s.cells[i];
    if (piece == EMPTY) continue;
    const uint8_t side = sideOf(piece) >> 3;
    const uint8_t type = typeOf(piece);
    const int8_t row = i / 8, col = i % 8;
    // 0 in a corner up to 6 in the middle four squares.
    const int center = (row < 4 ? row : 7 - row) + (col < 4 ? col : 7 - col);
    const int forward = side == 0 ? 7 - row : row;
    int value = VALUES[type];
    material[side] += value;
    if (type == PAWN) {
      value += forward * 4;
      if (col == 3 || col == 4) value += forward * 3;
    } else if (type == KNIGHT || type == BISHOP) {
      value += center * 6;
      if (forward == 0) value -= 15;  // still at home
    } else if (type == QUEEN) {
      value += center;
    } else if (type == ROOK && forward == 6) {
      value += 20;
    }
    score[side] += value;
  }
  for (uint8_t side = 0; side < 2; ++side) {
    const uint8_t square = s.king[side];
    const int8_t row = square / 8, col = square % 8;
    const int center = (row < 4 ? row : 7 - row) + (col < 4 ? col : 7 - col);
    if (material[side ^ 1] > 1500) {  // pieces about: stay tucked away
      score[side] -= center * 8;
      if (col <= 2 || col >= 6) score[side] += 25;
    } else {  // the ending: the king fights in the middle
      score[side] += center * 10;
    }
  }
  const uint8_t mine = s.turn >> 3;
  return score[mine] - score[mine ^ 1];
}

// How promising a move looks, so the best are searched first.
inline int promise(const State& s, const Move& m) {
  const uint8_t victim = typeOf(s.cells[m.to]);
  int value = 0;
  if (victim != EMPTY) {
    value = 1000 + VALUES[victim] - typeOf(s.cells[m.from]);
  }
  if (typeOf(s.cells[m.from]) == PAWN && (m.to < 8 || m.to >= 56)) {
    value += 900;
  }
  return value;
}

inline void bringBest(const State& s, Move* moves, uint16_t from,
                      uint16_t count) {
  uint16_t best = from;
  int bestValue = promise(s, moves[from]);
  for (uint16_t i = from + 1; i < count; ++i) {
    const int value = promise(s, moves[i]);
    if (value > bestValue) {
      best = i;
      bestValue = value;
    }
  }
  const Move swap = moves[from];
  moves[from] = moves[best];
  moves[best] = swap;
}

// Room the search keeps its move lists in.
struct Workspace {
  static constexpr uint16_t SIZE = 1536;
  Move moves[SIZE];
};

// The score for the side to move, looking `depth` half-moves ahead and then
// on through the captures (alpha-beta).
inline int search(const State& s, int8_t depth, int alpha, int beta,
                  Workspace& work, uint16_t used, uint8_t ply) {
  if (s.quiet >= 100) return 0;
  const bool check = depth > 0 && inCheck(s);
  const bool loud = depth <= 0;
  if (loud) {
    const int stand = evaluate(s);
    if (stand >= beta || ply >= 10) return stand;
    if (stand > alpha) alpha = stand;
  }
  if (used + MAX_MOVES > Workspace::SIZE) return evaluate(s);
  Move* const moves = work.moves + used;
  const uint16_t count = pseudoMoves(s, moves, loud);
  bool any = false;
  State next;
  for (uint16_t i = 0; i < count; ++i) {
    bringBest(s, moves, i, count);
    if (!tryMove(s, moves[i], next)) continue;
    any = true;
    const int score = -search(next, depth - 1, -beta, -alpha, work,
                              used + count, ply + 1);
    if (score > alpha) alpha = score;
    if (alpha >= beta) break;
  }
  if (!any && !loud) return check ? -20000 + ply : 0;
  return alpha;
}

// The computer's move, looking `depth` half-moves ahead; `pick(n)` returns a
// number below n, for variety among equally good moves. `seen` are earlier
// positions it should not casually go back to. There must be a legal move.
template <typename Pick>
Move computerMove(const State& s, uint8_t depth, Workspace& work,
                  const uint32_t* seen, uint8_t seenCount, const Pick& pick) {
  Move moves[MAX_MOVES];
  const uint16_t count = legalMoves(s, moves);
  Move best = moves[0];
  int bestScore = -INFINITE;
  uint16_t ties = 0;
  State next;
  for (uint16_t i = 0; i < count; ++i) {
    bringBest(s, moves, i, count);
    tryMove(s, moves[i], next);
    // One below the best so far, so equal moves still score exactly.
    int score = -search(next, depth - 1, -INFINITE,
                        ties == 0 ? INFINITE : -(bestScore - 1), work, 0, 1);
    const uint32_t mark = fingerprint(next);
    for (uint8_t k = 0; k < seenCount; ++k) {
      if (seen[k] == mark && score > bestScore - 1) {
        score -= 40;  // going in circles
        break;
      }
    }
    if (ties == 0 || score > bestScore) {
      bestScore = score;
      best = moves[i];
      ties = 1;
    } else if (score == bestScore && pick(++ties) == 0) {
      best = moves[i];
    }
  }
  return best;
}

}  // namespace chess
