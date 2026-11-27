#pragma once

#include <stdint.h>

// Checkers (American rules) and a small computer player, kept apart from the
// drawing. Pieces move diagonally forward and kings both ways, one square at a
// time. A jump must be taken, and the same piece keeps jumping while it can.
// Reaching the far row makes a king and ends the turn. A side with no move
// loses; 40 turns each with only kings moving and nothing taken is a draw.
namespace checkers {

constexpr uint8_t SIZE = 8;
constexpr uint8_t CELLS = SIZE * SIZE;
constexpr uint8_t EMPTY = 0;
constexpr uint8_t FIRST = 1;   // starts at the bottom and moves up
constexpr uint8_t SECOND = 2;  // starts at the top and moves down
constexpr uint8_t KING = 4;
constexpr uint8_t MAX_MOVES = 48;
constexpr uint8_t DRAW_PLIES = 80;

struct Move {
  uint8_t from;
  uint8_t to;
  int8_t taken;  // the cell jumped over, or -1
};

struct State {
  uint8_t cells[CELLS];  // row * SIZE + column, row 0 at the top
  uint8_t turn;
  int8_t jumper;  // the piece that has to go on jumping, or -1
  uint8_t quiet;  // turns without a capture or a plain piece moving
};

inline uint8_t sideOf(uint8_t piece) { return piece & 3; }
inline uint8_t other(uint8_t side) { return side == FIRST ? SECOND : FIRST; }

inline void reset(State& s) {
  for (uint8_t i = 0; i < CELLS; ++i) {
    const uint8_t row = i / SIZE, col = i % SIZE;
    s.cells[i] = EMPTY;
    if ((row + col) % 2 == 0) continue;
    if (row < 3) s.cells[i] = SECOND;
    if (row > 4) s.cells[i] = FIRST;
  }
  s.turn = FIRST;
  s.jumper = -1;
  s.quiet = 0;
}

// Add the moves of the piece at `from`: only jumps, or only plain steps.
inline void pieceMoves(const State& s, uint8_t from, bool jumps, Move* out,
                       uint8_t& count) {
  const uint8_t piece = s.cells[from];
  const int8_t row = from / SIZE, col = from % SIZE;
  for (int8_t dr = -1; dr <= 1; dr += 2) {
    if (!(piece & KING) && dr != (sideOf(piece) == FIRST ? -1 : 1)) continue;
    for (int8_t dc = -1; dc <= 1; dc += 2) {
      const int8_t reach = jumps ? 2 : 1;
      const int8_t r = row + dr * reach, c = col + dc * reach;
      if (r < 0 || r >= SIZE || c < 0 || c >= SIZE) continue;
      const uint8_t to = r * SIZE + c;
      if (s.cells[to] != EMPTY) continue;
      int8_t taken = -1;
      if (jumps) {
        taken = (row + dr) * SIZE + col + dc;
        if (sideOf(s.cells[taken]) != other(sideOf(piece))) continue;
      }
      if (count < MAX_MOVES) out[count++] = {from, to, taken};
    }
  }
}

// Every step the side to move may take now; returns how many.
inline uint8_t legalMoves(const State& s, Move* out) {
  uint8_t count = 0;
  if (s.jumper >= 0) {
    pieceMoves(s, s.jumper, true, out, count);
    return count;
  }
  for (uint8_t pass = 0; pass < 2 && count == 0; ++pass) {
    for (uint8_t i = 0; i < CELLS; ++i) {
      if (sideOf(s.cells[i]) == s.turn) {
        pieceMoves(s, i, pass == 0, out, count);
      }
    }
  }
  return count;
}

inline void apply(State& s, const Move& m) {
  uint8_t piece = s.cells[m.from];
  const bool plain = !(piece & KING);
  s.cells[m.from] = EMPTY;
  if (m.taken >= 0) s.cells[m.taken] = EMPTY;
  const uint8_t farRow = sideOf(piece) == FIRST ? 0 : SIZE - 1;
  const bool crowned = plain && m.to / SIZE == farRow;
  if (crowned) piece |= KING;
  s.cells[m.to] = piece;
  if (m.taken >= 0 || plain) {
    s.quiet = 0;
  } else if (s.quiet < DRAW_PLIES) {
    ++s.quiet;
  }
  if (m.taken >= 0 && !crowned) {
    Move more[4];
    uint8_t count = 0;
    pieceMoves(s, m.to, true, more, count);
    if (count > 0) {
      s.jumper = m.to;
      return;
    }
  }
  s.jumper = -1;
  s.turn = other(s.turn);
}

// 0 while the game goes on, FIRST or SECOND for the winner, 3 for a draw.
inline uint8_t result(const State& s) {
  Move moves[MAX_MOVES];
  if (legalMoves(s, moves) == 0) return other(s.turn);
  return s.quiet >= DRAW_PLIES ? 3 : 0;
}

// How good the board looks to `side`.
inline int evaluate(const State& s, uint8_t side) {
  int score = 0;
  for (uint8_t i = 0; i < CELLS; ++i) {
    const uint8_t piece = s.cells[i];
    if (piece == EMPTY) continue;
    const uint8_t row = i / SIZE, col = i % SIZE;
    const bool first = sideOf(piece) == FIRST;
    int value;
    if (piece & KING) {
      value = 170;
      if (row >= 2 && row <= 5 && col >= 2 && col <= 5) value += 8;
    } else {
      const uint8_t advanced = first ? SIZE - 1 - row : row;
      value = 100 + advanced * 3;
      if (advanced == 0) value += 8;  // guarding the back row
      if (col >= 2 && col <= 5) value += 2;
    }
    score += sideOf(piece) == side ? value : -value;
  }
  return score;
}

// The score for the side to move, looking `depth` turns ahead (alpha-beta).
inline int search(const State& s, uint8_t depth, int alpha, int beta) {
  Move moves[MAX_MOVES];
  const uint8_t count = legalMoves(s, moves);
  if (count == 0) return -10000 - depth;  // sooner losses are worse
  if (s.quiet >= DRAW_PLIES) return 0;
  // A capture is never left half finished.
  if (depth == 0 && moves[0].taken < 0) return evaluate(s, s.turn);
  for (uint8_t i = 0; i < count; ++i) {
    State next = s;
    apply(next, moves[i]);
    const bool again = next.turn == s.turn;
    const uint8_t left = again || depth == 0 ? depth : depth - 1;
    const int score = again ? search(next, left, alpha, beta)
                            : -search(next, left, -beta, -alpha);
    if (score > alpha) alpha = score;
    if (alpha >= beta) break;
  }
  return alpha;
}

// The computer's step; `pick(n)` returns a number below n, for variety among
// equally good moves. The state must have at least one legal move.
template <typename Pick>
Move computerMove(const State& s, uint8_t depth, const Pick& pick) {
  Move moves[MAX_MOVES];
  const uint8_t count = legalMoves(s, moves);
  Move best = moves[0];
  int bestScore = -30000;
  uint8_t ties = 0;
  for (uint8_t i = 0; i < count; ++i) {
    State next = s;
    apply(next, moves[i]);
    const bool again = next.turn == s.turn;
    const uint8_t left = again ? depth : depth - 1;
    const int score = again ? search(next, left, -30000, 30000)
                            : -search(next, left, -30000, 30000);
    if (score > bestScore) {
      bestScore = score;
      best = moves[i];
      ties = 1;
    } else if (score == bestScore && pick(++ties) == 0) {
      best = moves[i];
    }
  }
  return best;
}

}  // namespace checkers
