/*
  Copyright (c) 2013-2017 Ronald de Man
  This file may be redistributed and/or modified without restrictions.

  tbprobe.cpp contains the Stockfish-specific routines of the
  tablebase probing code. It should be relatively easy to adapt
  this code to other chess engines.
*/

#include "position.h"
#include "movegen.h"
#include "bitboard.h"
#include "search.h"
#include "uci.h"

#include "tbprobe.h"
#include "tbcore.h"

#include "tbcore.c"

extern Key mat_key[16];

int TB_MaxCardinality = 0, TB_MaxCardinalityDTM = 0;
extern int TB_CardinalityDTM;

// Given a position with 6 or fewer pieces, produce a text string
// of the form KQPvKRP, where "KQP" represents the white pieces if
// mirror == 0 and the black pieces if mirror == 1.
static void prt_str(Pos *pos, char *str, int mirror)
{
  int color = !mirror ? WHITE : BLACK;

  for (int pt = KING; pt >= PAWN; pt--)
    for (int i = popcount(pieces_cp(color, pt)); i > 0; i--)
      *str++ = pchr[6 - pt];
  *str++ = 'v';
  color ^= 1;
  for (int pt = KING; pt >= PAWN; pt--)
    for (int i = popcount(pieces_cp(color, pt)); i > 0; i--)
      *str++ = pchr[6 - pt];
  *str++ = 0;
}

// Given a position, produce a 64-bit material signature key.
// If the engine supports such a key, it should equal the engine's key.
static Key calc_key(Pos *pos, int mirror)
{
  Key key = 0;

  int color = !mirror ? WHITE : BLACK;
  for (int pt = PAWN; pt <= KING; pt++)
    key += mat_key[pt] * popcount(pieces_cp(color, pt));
  color ^= 1;
  for (int pt = PAWN; pt <= KING; pt++)
    key += mat_key[pt + 8] * popcount(pieces_cp(color, pt));

  return key;
}

// Produce a 64-bit material key corresponding to the material combination
// defined by pcs[16], where pcs[1], ..., pcs[6] is the number of white
// pawns, ..., kings and pcs[9], ..., pcs[14] is the number of black
// pawns, ..., kings.
static Key calc_key_from_pcs(int *pcs, int mirror)
{
  Key key = 0;

  int color = !mirror ? 0 : 8;
  for (int i = W_PAWN; i <= B_KING; i++)
    key += mat_key[i] * pcs[i ^ color];

  return key;
}

// Produce a 64-bit material key corresponding to the material combination
// piece[0], ..., piece[num - 1], where each value corresponds to a piece
// (1-6 for white pawn-king, 9-14 for black pawn-king).
static Key calc_key_from_pieces(uint8_t *piece, int num, int mirror)
{
  Key key = 0;

  int color = !mirror ? 0 : 8;
  for (int i = 0; i < num; i++)
    if (piece[i])
      key += mat_key[piece[i] ^ color];

  return key;
}

// probe_wdl_table and probe_dtz_table require similar adaptations.
static int probe_wdl_table(Pos *pos, int *success)
{
  struct TBEntry *ptr;
  struct TBHashEntry *ptr2;
  uint64_t idx;
  int i;
  uint8_t res;
  int p[TBPIECES];

  // Obtain the position's material signature key.
  Key key = pos_material_key();

  // Test for KvK.
  if (key == 2ULL)
    return 0;

  ptr2 = TB_hash[key >> (64 - TBHASHBITS)];
  for (i = 0; i < HSHMAX; i++)
    if (ptr2[i].key == key) break;
  if (i == HSHMAX) {
    *success = 0;
    return 0;
  }

  ptr = ptr2[i].ptr;
  // With the help of C11 atomics, we implement double-checked locking
  // correctly.
  if (!atomic_load_explicit(&ptr->ready, memory_order_acquire)) {
    LOCK(TB_mutex);
    if (!atomic_load_explicit(&ptr->ready, memory_order_relaxed)) {
      char str[16];
      prt_str(pos, str, ptr->key != key);
      if (!init_table(ptr, str, 0)) {
        ptr2[i].key = 0ULL;
        *success = 0;
        UNLOCK(TB_mutex);
        return 0;
      }
      atomic_store_explicit(&ptr->ready, 1, memory_order_release);
    }
    UNLOCK(TB_mutex);
  }

  int bside, mirror, cmirror;
  if (!ptr->symmetric) {
    if (key != ptr->key) {
      cmirror = 8;
      mirror = 0x38;
      bside = (pos_stm() == WHITE);
    } else {
      cmirror = mirror = 0;
      bside = !(pos_stm() == WHITE);
    }
  } else {
    cmirror = pos_stm() == WHITE ? 0 : 8;
    mirror = pos_stm() == WHITE ? 0 : 0x38;
    bside = 0;
  }

  // p[i] is to contain the square 0-63 (A1-H8) for a piece of type
  // pc[i] ^ cmirror, where 1 = white pawn, ..., 14 = black king.
  // Pieces of the same type are guaranteed to be consecutive.
  if (!ptr->has_pawns) {
    struct TBEntry_piece *entry = (struct TBEntry_piece *)ptr;
    uint8_t *pc = entry->pieces[bside];
    for (i = 0; i < entry->num;) {
      Bitboard bb = pieces_cp((pc[i] ^ cmirror) >> 3, pc[i] & 0x07);
      do {
        p[i++] = pop_lsb(&bb);
      } while (bb);
    }
    idx = encode_piece(entry, entry->norm[bside], p, entry->factor[bside]);
    res = *decompress_pairs(entry->precomp[bside], idx);
  } else {
    struct TBEntry_pawn *entry = (struct TBEntry_pawn *)ptr;
    int k = entry->file[0].pieces[0][0] ^ cmirror;
    Bitboard bb = pieces_cp(k >> 3, k & 0x07);
    i = 0;
    do {
      p[i++] = pop_lsb(&bb) ^ mirror;
    } while (bb);
    int f = pawn_file(entry, p);
    uint8_t *pc = entry->file[f].pieces[bside];
    for (; i < entry->num;) {
      bb = pieces_cp((pc[i] ^ cmirror) >> 3, pc[i] & 0x07);
      do {
        assume(i < TBPIECES); // Suppress a bogus warning.
        p[i++] = pop_lsb(&bb) ^ mirror;
      } while (bb);
    }
    idx = encode_pawn(entry, entry->file[f].norm[bside], p, entry->file[f].factor[bside]);
    res = *decompress_pairs(entry->file[f].precomp[bside], idx);
  }

  return res - 2;
}

static int probe_dtm_table(Pos *pos, int won, int *success)
{
  struct TBEntry *ptr;
  struct TBHashEntry *ptr2;
  uint64_t idx;
  int i;
  int res;
  int p[TBPIECES];

  // Obtain the position's material signature key.
  Key key = pos_material_key();

  // Test for KvK.
  if (key == 2ULL)
    return 0;

  ptr2 = TB_hash[key >> (64 - TBHASHBITS)];
  for (i = 0; i < HSHMAX; i++)
    if (ptr2[i].key == key) break;
  if (i == HSHMAX) {
    *success = 0;
    return 0;
  }

  ptr = ptr2[i].dtm_ptr;
  if (!ptr) {
    *success = 0;
    return 0;
  }

  // With the help of C11 atomics, we implement double-checked locking
  // correctly.
  if (!atomic_load_explicit(&ptr->ready, memory_order_acquire)) {
    LOCK(TB_mutex);
    if (!atomic_load_explicit(&ptr->ready, memory_order_relaxed)) {
      char str[16];
      prt_str(pos, str, ptr->key != key);
      if (!init_table(ptr, str, 1)) {
        ptr->data = NULL;
        ptr2[i].dtm_ptr = NULL;
        *success = 0;
        UNLOCK(TB_mutex);
        return 0;
      }
      atomic_store_explicit(&ptr->ready, 1, memory_order_release);
    }
    UNLOCK(TB_mutex);
  }

  int bside, mirror, cmirror;
  if (!ptr->symmetric) {
    if (key != ptr->key) {
      cmirror = 8;
      mirror = 0x38;
      bside = (pos_stm() == WHITE);
    } else {
      cmirror = mirror = 0;
      bside = !(pos_stm() == WHITE);
    }
  } else {
    cmirror = pos_stm() == WHITE ? 0 : 8;
    mirror = pos_stm() == WHITE ? 0 : 0x38;
    bside = 0;
  }

  // p[i] is to contain the square 0-63 (A1-H8) for a piece of type
  // pc[i] ^ cmirror, where 1 = white pawn, ..., 14 = black king.
  // Pieces of the same type are guaranteed to be consecutive.
  if (!ptr->has_pawns) {
    struct DTMEntry_piece *entry = (struct DTMEntry_piece *)ptr;
    uint8_t *pc = entry->pieces[bside];
    for (i = 0; i < entry->num;) {
      Bitboard bb = pieces_cp((pc[i] ^ cmirror) >> 3, pc[i] & 0x07);
      do {
        p[i++] = pop_lsb(&bb);
      } while (bb);
    }
    idx = encode_piece((struct TBEntry_piece *)entry, entry->norm[bside], p, entry->factor[bside]);
    uint8_t *w = decompress_pairs(entry->precomp[bside], idx);
    res = ((w[1] & 0x0f) << 8) | w[0];
    if (!entry->loss_only)
      res = entry->map[entry->map_idx[bside][won] + res];
  } else {
    struct DTMEntry_pawn *entry = (struct DTMEntry_pawn *)ptr;
    int k = entry->rank[0].pieces[0][0] ^ cmirror;
    Bitboard bb = pieces_cp(k >> 3, k & 0x07);
    i = 0;
    do {
      p[i++] = pop_lsb(&bb) ^ mirror;
    } while (bb);
    int r = pawn_rank((struct TBEntry_pawn2 *)entry, p);
    uint8_t *pc = entry->rank[r].pieces[bside];
    for (; i < entry->num;) {
      bb = pieces_cp((pc[i] ^ cmirror) >> 3, pc[i] & 0x07);
      do {
        assume(i < TBPIECES); // Suppress a bogus warning.
        p[i++] = pop_lsb(&bb) ^ mirror;
      } while (bb);
    }
    idx = encode_pawn2((struct TBEntry_pawn2 *)entry, entry->rank[r].norm[bside], p, entry->rank[r].factor[bside]);
    uint8_t *w = decompress_pairs(entry->rank[r].precomp[bside], idx);
    res = ((w[1] & 0x0f) << 8) | w[0];
    if (!entry->loss_only)
      res = entry->map[entry->map_idx[r][bside][won] + res];
  }

  return res;
}

// The value of wdl MUST correspond to the WDL value of the position without
// en passant rights.
static int probe_dtz_table(Pos *pos, int wdl, int *success)
{
  struct TBEntry *ptr;
  uint64_t idx;
  int i;
  uint32_t res;
  int p[TBPIECES];

  // Obtain the position's material signature key.
  Key key = pos_material_key();

  if (DTZ_table[0].key1 != key && DTZ_table[0].key2 != key) {
    for (i = 1; i < DTZ_ENTRIES; i++)
      if (DTZ_table[i].key1 == key || DTZ_table[i].key2 == key) break;
    if (i < DTZ_ENTRIES) {
      struct DTZTableEntry table_entry = DTZ_table[i];
      for (; i > 0; i--)
        DTZ_table[i] = DTZ_table[i - 1];
      DTZ_table[0] = table_entry;
    } else {
      struct TBHashEntry *ptr2 = TB_hash[key >> (64 - TBHASHBITS)];
      for (i = 0; i < HSHMAX; i++)
        if (ptr2[i].key == key) break;
      if (i == HSHMAX) {
        *success = 0;
        return 0;
      }
      ptr = ptr2[i].ptr;
      char str[16];
      int mirror = (ptr->key != key);
      prt_str(pos, str, mirror);
      if (DTZ_table[DTZ_ENTRIES - 1].entry)
        free_dtz_entry(DTZ_table[DTZ_ENTRIES-1].entry);
      for (i = DTZ_ENTRIES - 1; i > 0; i--)
        DTZ_table[i] = DTZ_table[i - 1];
      load_dtz_table(str, calc_key(pos, mirror), calc_key(pos, !mirror));
    }
  }

  ptr = DTZ_table[0].entry;
  if (!ptr) {
    *success = 0;
    return 0;
  }

  int bside, mirror, cmirror;
  if (!ptr->symmetric) {
    if (key != ptr->key) {
      cmirror = 8;
      mirror = 0x38;
      bside = (pos_stm() == WHITE);
    } else {
      cmirror = mirror = 0;
      bside = !(pos_stm() == WHITE);
    }
  } else {
    cmirror = pos_stm() == WHITE ? 0 : 8;
    mirror = pos_stm() == WHITE ? 0 : 0x38;
    bside = 0;
  }

  if (!ptr->has_pawns) {
    struct DTZEntry_piece *entry = (struct DTZEntry_piece *)ptr;
    if ((entry->flags & 1) != bside && !entry->symmetric) {
      *success = -1;
      return 0;
    }
    uint8_t *pc = entry->pieces;
    for (i = 0; i < entry->num;) {
      Bitboard bb = pieces_cp((pc[i] ^ cmirror) >> 3, pc[i] & 0x07);
      do {
        p[i++] = pop_lsb(&bb);
      } while (bb);
    }
    idx = encode_piece((struct TBEntry_piece *)entry, entry->norm, p, entry->factor);
    res = *decompress_pairs(entry->precomp, idx);

    if (entry->flags & 2)
      res = entry->map[entry->map_idx[wdl_to_map[wdl + 2]] + res];

    if (!(entry->flags & pa_flags[wdl + 2]) || (wdl & 1))
      res *= 2;
  } else {
    struct DTZEntry_pawn *entry = (struct DTZEntry_pawn *)ptr;
    int k = entry->file[0].pieces[0] ^ cmirror;
    Bitboard bb = pieces_cp(k >> 3, k & 0x07);
    i = 0;
    do {
      p[i++] = pop_lsb(&bb) ^ mirror;
    } while (bb);
    int f = pawn_file((struct TBEntry_pawn *)entry, p);
    if ((entry->flags[f] & 1) != bside) {
      *success = -1;
      return 0;
    }
    uint8_t *pc = entry->file[f].pieces;
    for (; i < entry->num;) {
      bb = pieces_cp((pc[i] ^ cmirror) >> 3, pc[i] & 0x07);
      do {
        assume(i < TBPIECES); // Suppress a bogus warning.
        p[i++] = pop_lsb(&bb) ^ mirror;
      } while (bb);
    }
    idx = encode_pawn((struct TBEntry_pawn *)entry, entry->file[f].norm, p, entry->file[f].factor);
    res = *decompress_pairs(entry->file[f].precomp, idx);

    if (entry->flags[f] & 2)
      res = entry->map[entry->map_idx[f][wdl_to_map[wdl + 2]] + res];

    if (!(entry->flags[f] & pa_flags[wdl + 2]) || (wdl & 1))
      res *= 2;
  }

  return res;
}

// Add underpromotion captures to list of captures.
static ExtMove *add_underprom_caps(Pos *pos, ExtMove *m, ExtMove *end)
{
  ExtMove *extra = end;

  for (; m < end; m++) {
    Move move = m->move;
    if (type_of_m(move) == PROMOTION && piece_on(to_sq(move))) {
      (*extra++).move = (Move)(move - (1 << 12));
      (*extra++).move = (Move)(move - (2 << 12));
      (*extra++).move = (Move)(move - (3 << 12));
    }
  }

  return extra;
}

// This will not be called for positions with en passant captures.
static int probe_ab(Pos *pos, int alpha, int beta, int *success)
{
  assert(ep_square() == 0);

  int v;
  ExtMove *m = (pos->st-1)->endMoves;
  ExtMove *end;

  // Generate (at least) all legal captures including (under)promotions.
  // It is OK to generate more, as long as they are filtered out below.
  if (!pos_checkers()) {
    end = generate_captures(pos, m);
    // Since underpromotion captures are not included, we need to add them.
    end = add_underprom_caps(pos, m, end);
  } else
    end = generate_evasions(pos, m);
  pos->st->endMoves = end;

  for (; m < end; m++) {
    Move move = m->move;
    if (!is_capture(pos, move) || !is_legal(pos, move))
      continue;
    do_move(pos, move, gives_check(pos, pos->st, move));
    v = -probe_ab(pos, -beta, -alpha, success);
    undo_move(pos, move);
    if (*success == 0) return 0;
    if (v > alpha) {
      if (v >= beta)
        return v;
      alpha = v;
    }
  }

  v = probe_wdl_table(pos, success);

  return alpha >= v ? alpha : v;
}

// Probe the WDL table for a particular position.
//
// If *success != 0, the probe was successful.
//
// If *success == 2, the position has a winning capture, or the position
// is a cursed win and has a cursed winning capture, or the position
// has an ep capture as only best move.
// This is used in probe_dtz().
//
// The return value is from the point of view of the side to move:
// -2 : loss
// -1 : loss, but draw under 50-move rule
//  0 : draw
//  1 : win, but draw under 50-move rule
//  2 : win
int TB_probe_wdl(Pos *pos, int *success)
{
  *success = 1;

  // Generate (at least) all legal en passant captures.
  ExtMove *m = (pos->st-1)->endMoves;
  ExtMove *end;

  // Generate (at least) all legal captures including (under)promotions.
  if (!pos_checkers()) {
    end = generate_captures(pos, m);
    end = add_underprom_caps(pos, m, end);
  } else
    end = generate_evasions(pos, m);
  pos->st->endMoves = end;

  int best_cap = -3, best_ep = -3;

  // We do capture resolution, letting best_cap keep track of the best
  // capture without ep rights and letting best_ep keep track of still
  // better ep captures if they exist.

  for (; m < end; m++) {
    Move move = m->move;
    if (!is_capture(pos, move) || !is_legal(pos, move))
      continue;
    do_move(pos, move, gives_check(pos, pos->st, move));
    int v = -probe_ab(pos, -2, -best_cap, success);
    undo_move(pos, move);
    if (*success == 0) return 0;
    if (v > best_cap) {
      if (v == 2) {
	*success = 2;
	return 2;
      }
      if (type_of_m(move) != ENPASSANT)
        best_cap = v;
      else if (v > best_ep)
        best_ep = v;
    }
  }

  int v = probe_wdl_table(pos, success);
  if (*success == 0) return 0;

  // Now max(v, best_cap) is the WDL value of the position without ep rights.
  // If the position without ep rights is not stalemate or no ep captures
  // exist, then the value of the position is max(v, best_cap, best_ep).
  // If the position without ep rights is stalemate and best_ep > -3,
  // then the value of the position is best_ep (and we will have v == 0).

  if (best_ep > best_cap) {
    if (best_ep > v) { // ep capture (possibly cursed losing) is best.
      *success = 2;
      return best_ep;
    }
    best_cap = best_ep;
  }

  // Now max(v, best_cap) is the WDL value of the position unless
  // the position without ep rights is stalemate and best_ep > -3.

  if (best_cap >= v) {
    // No need to test for the stalemate case here: either there are
    // non-ep captures, or best_cap == best_ep >= v anyway.
    *success = 1 + (best_cap > 0);
    return best_cap;
  }

  // Now handle the stalemate case.
  if (best_ep > -3 && v == 0) {
    // Check for stalemate in the position with ep captures.
    for (m = (pos->st-1)->endMoves; m < end; m++) {
      Move move = m->move;
      if (type_of_m(move) == ENPASSANT) continue;
      if (is_legal(pos, move)) break;
    }
    if (m == end && !pos_checkers()) {
      end = generate_quiets(pos, end);
      for (; m < end; m++) {
        Move move = m->move;
        if (is_legal(pos, move))
          break;
      }
    }
    if (m == end) { // Stalemate detected.
      *success = 2;
      return best_ep;
    }
  }

  // Stalemate / en passant not an issue, so v is the correct value.

  return v;
}

#if 0
// This will not be called for positions with en passant captures
static Value probe_dtm_dc(Pos *pos, int won, int *success)
{
  assert(ep_square() == 0);

  Value v, best_cap = -VALUE_INFINITE;

  ExtMove *end, *m = (pos->st-1)->endMoves;

  // Generate at least all legal captures including (under)promotions
  if (!pos_checkers()) {
    end = generate_captures(pos, m);
    end = add_underprom_caps(pos, m, end);
  } else
    end = generate_evasions(pos, m);
  pos->st->endMoves = end;

  for (; m < end; m++) {
    Move move = m->move;
    if (!is_capture(pos, move) || !is_legal(pos, move))
      continue;
    do_move(pos, move, gives_check(pos, pos->st, move));
    if (!won)
      v = -probe_dtm_dc(pos, 1, success) + 1;
    else if (probe_ab(pos, -1, 0, success) < 0 && *success)
      v = -probe_dtm_dc(pos, 0, success) - 1;
    else
      v = -VALUE_INFINITE;
    undo_move(pos, move);
    best_cap = max(best_cap, v);
    if (*success == 0) return 0;
  }

  int dtm = probe_dtm_table(pos, won, success);
  v = won ? VALUE_MATE - 2 * dtm + 1 : -VALUE_MATE + 2 * dtm;

  return max(best_cap, v);
}
#endif

static Value probe_dtm_win(Pos *pos, int *success);

// Probe a position known to lose by probing the DTM table and looking
// at captures.
static Value probe_dtm_loss(Pos *pos, int *success)
{
  Value v, best = -VALUE_INFINITE, num_ep = 0;

  ExtMove *end, *m = (pos->st-1)->endMoves;

  // Generate at least all legal captures including (under)promotions
  end = pos_checkers() ? generate_evasions(pos, m)
                       : add_underprom_caps(pos, m, generate_captures(pos, m));
  pos->st->endMoves = end;

  for (; m < end; m++) {
    Move move = m->move;
    if (!is_capture(pos, move) || !is_legal(pos, move))
      continue;
    if (type_of_m(move) == ENPASSANT)
      num_ep++;
    do_move(pos, move, gives_check(pos, pos->st, move));
    v = -probe_dtm_win(pos, success) + 1;
    undo_move(pos, move);
    best = max(best, v);
    if (*success == 0)
      return 0;
  }

  // If there are en passant captures, the position without ep rights
  // may be a stalemate. If it is, we must avoid probing the DTM table.
  if (num_ep != 0 && generate_legal(pos, m) == m + num_ep)
    return best;

  v = -VALUE_MATE + 2 * probe_dtm_table(pos, 0, success);
  return max(best, v);
}

static Value probe_dtm_win(Pos *pos, int *success)
{
  Value v, best = -VALUE_INFINITE;

  // Generate all moves
  ExtMove *end, *m = (pos->st-1)->endMoves;
  end = pos_checkers() ? generate_evasions(pos, m)
                       : generate_non_evasions(pos, m);
  pos->st->endMoves = end;

  // Perform a 1-ply search
  for (; m < end; m++) {
    Move move = m->move;
    if (!is_legal(pos, move))
      continue;
    do_move(pos, move, gives_check(pos, pos->st, move));
    if (   (ep_square() ? TB_probe_wdl(pos, success)
                        : probe_ab(pos, -1, 0, success)) < 0
        && *success)
      v = -probe_dtm_loss(pos, success) - 1;
    else
      v = -VALUE_INFINITE;
    undo_move(pos, move);
    best = max(best, v);
    if (*success == 0) return 0;
  }

  return best;
}

Value TB_probe_dtm(Pos *pos, int wdl, int *success)
{
  assert(wdl != 0);

  *success = 1;

  return wdl > 0 ? probe_dtm_win(pos, success)
                 : probe_dtm_loss(pos, success);
}

#if 0
// To be called only for non-drawn positions.
Value TB_probe_dtm2(Pos *pos, int wdl, int *success)
{
  assert(wdl != 0);

  *success = 1;
  Value v, best_cap = -VALUE_INFINITE, best_ep = -VALUE_INFINITE;

  ExtMove *end, *m = (pos->st-1)->endMoves;

  // Generate at least all legal captures including (under)promotions
  if (!pos_checkers()) {
    end = generate_captures(pos, m);
    end = add_underprom_caps(pos, m, end);
  } else
    end = generate_evasions(pos, m);
  pos->st->endMoves = end;

  // Resolve captures, letting best_cap keep track of the best non-ep
  // capture and letting best_ep keep track of the best ep capture.
  for (; m < end; m++) {
    Move move = m->move;
    if (!is_capture(pos, move) || !is_legal(pos, move))
      continue;
    do_move(pos, move, gives_check(pos, pos->st, move));
    if (wdl < 0)
      v = -probe_dtm_dc(pos, 1, success) + 1;
    else if (probe_ab(pos, -1, 0, success) < 0 && *success)
      v = -probe_dtm_dc(pos, 0, success) - 1;
    else
      v = -VALUE_MATE;
    undo_move(pos, move);
    if (type_of_m(move) == ENPASSANT)
      best_ep = max(best_ep, v);
    else
      best_cap = max(best_cap, v);
    if (*success == 0)
      return 0;
  }

  // If there are en passant captures, we have to determine the WDL value
  // for the position without ep rights if it might be different.
  if (best_ep > -VALUE_INFINITE && (best_ep < 0 || best_cap < 0)) {
    assert(ep_square() != 0);
    uint8_t s = pos->st->epSquare;
    pos->st->epSquare = 0;
    wdl = probe_ab(pos, -2, 2, success);
    pos->st->epSquare = s;
    if (*success == 0)
      return 0;
    if (wdl == 0)
      return best_ep;
  }

  best_cap = max(best_cap, best_ep);
  int dtm = probe_dtm_table(pos, wdl > 0, success);
  v = wdl > 0 ? VALUE_MATE - 2 * dtm + 1 : -VALUE_MATE + 2 * dtm;
  return max(best_cap, v);
}
#endif

static int wdl_to_dtz[] = {
  -1, -101, 0, 101, 1
};

// Probe the DTZ table for a particular position.
// If *success != 0, the probe was successful.
// The return value is from the point of view of the side to move:
//         n < -100 : loss, but draw under 50-move rule
// -100 <= n < -1   : loss in n ply (assuming 50-move counter == 0)
//         0        : draw
//     1 < n <= 100 : win in n ply (assuming 50-move counter == 0)
//   100 < n        : win, but draw under 50-move rule
//
// If the position mate, -1 is returned instead of 0.
//
// The return value n can be off by 1: a return value -n can mean a loss
// in n+1 ply and a return value +n can mean a win in n+1 ply. This
// cannot happen for tables with positions exactly on the "edge" of
// the 50-move rule.
//
// This means that if dtz > 0 is returned, the position is certainly
// a win if dtz + 50-move-counter <= 99. Care must be taken that the engine
// picks moves that preserve dtz + 50-move-counter <= 99.
//
// If n = 100 immediately after a capture or pawn move, then the position
// is also certainly a win, and during the whole phase until the next
// capture or pawn move, the inequality to be preserved is
// dtz + 50-movecounter <= 100.
//
// In short, if a move is available resulting in dtz + 50-move-counter <= 99,
// then do not accept moves leading to dtz + 50-move-counter == 100.
//
int TB_probe_dtz(Pos *pos, int *success)
{
  int wdl = TB_probe_wdl(pos, success);
  if (*success == 0) return 0;

  // If draw, then dtz = 0.
  if (wdl == 0) return 0;

  // Check for winning capture or en passant capture as only best move.
  if (*success == 2)
    return wdl_to_dtz[wdl + 2];

  ExtMove *end, *m = (pos->st-1)->endMoves;

  // If winning, check for a winning pawn move.
  if (wdl > 0) {
    // Generate at least all legal non-capturing pawn moves
    // including non-capturing promotions.
    // (The following calls in fact generate all moves.)
    if (!pos_checkers())
      end = generate_non_evasions(pos, m);
    else
      end = generate_evasions(pos, m);
    pos->st->endMoves = end;

    for (; m < end; m++) {
      Move move = m->move;
      if (type_of_p(moved_piece(move)) != PAWN || is_capture(pos, move)
                || !is_legal(pos, move))
        continue;
      do_move(pos, move, gives_check(pos, pos->st, move));
      int v = -TB_probe_wdl(pos, success);
      undo_move(pos, move);
      if (*success == 0) return 0;
      if (v == wdl)
        return wdl_to_dtz[wdl + 2];
    }
  }

  // If we are here, we know that the best move is not an ep capture.
  // In other words, the value of wdl corresponds to the WDL value of
  // the position without ep rights. It is therefore safe to probe the
  // DTZ table with the current value of wdl.

  int dtz = probe_dtz_table(pos, wdl, success);
  if (*success >= 0)
    return wdl_to_dtz[wdl + 2] + ((wdl > 0) ? dtz : -dtz);

  // *success < 0 means we need to probe DTZ for the other side to move.
  int best;
  if (wdl > 0) {
    best = INT32_MAX;
    // If wdl > 0, we have already generated all moves.
    m = (pos->st-1)->endMoves;
  } else {
    // If (cursed) loss, the worst case is a losing capture or pawn move
    // as the "best" move, leading to dtz of -1 or -101.
    // In case of mate, this will cause -1 to be returned.
    best = wdl_to_dtz[wdl + 2];
    // If wdl < 0, we still have to generate all moves.
    if (!pos_checkers())
      end = generate_non_evasions(pos, m);
    else
      end = generate_evasions(pos, m);
    pos->st->endMoves = end;
  }

  for (; m < end; m++) {
    Move move = m->move;
    // We can skip pawn moves and captures.
    // If wdl > 0, we already caught them. If wdl < 0, the initial value
    // of best already takes account of them.
    if (is_capture(pos, move) || type_of_p(moved_piece(move)) == PAWN
              || !is_legal(pos, move))
      continue;
    do_move(pos, move, gives_check(pos, pos->st, move));
    int v = -TB_probe_dtz(pos, success);
    undo_move(pos, move);
    if (*success == 0) return 0;
    if (wdl > 0) {
      if (v > 0 && v + 1 < best)
        best = v + 1;
    } else {
      if (v - 1 < best)
        best = v - 1;
    }
  }
  return best;
}

// Use the DTZ tables to rank and score all root moves in the list.
// A return value of 0 means that not all probes were successful.
int TB_root_probe_dtz(Pos *pos, RootMoves *rm)
{
  int v, success;

  // Obtain 50-move counter for the root position.
  int cnt50 = pos_rule50_count();

  // Check whether a position was repeated since the last zeroing move.
  // In that case, we need to be careful and play DTZ-optimal moves if
  // winning.
  int rep = pos->hasRepeated;

  // The border between draw and win lies at rank 1 or rank 900, depending
  // on whether the 50-move rule is used.
  int bound = option_value(OPT_SYZ_50_MOVE) ? 900 : 1;

  // Probe, rank and score each move.
  pos->st->endMoves = (pos->st-1)->endMoves;
  for (int i = 0; i < rm->size; i++) {
    RootMove *m = &rm->move[i];
    do_move(pos, m->pv[0], gives_check(pos, pos->st, m->pv[0]));

    // Calculate dtz for the current move counting from the root position.
    if (pos_rule50_count() == 0) {
      // If the move resets the 50-move counter, dtz is -101/-1/0/1/101.
      v = -TB_probe_wdl(pos, &success);
      v = wdl_to_dtz[v + 2];
    } else {
      // Otherwise, take dtz for the new position and correct by 1 ply.
      v = -TB_probe_dtz(pos, &success);
      if (v > 0) v++;
      else if (v < 0) v--;
    }
    // Make sure that a mating move gets value 1.
    if (pos_checkers() && v == 2) {
      if (generate_legal(pos, (pos->st-1)->endMoves) == (pos->st-1)->endMoves)
        v = 1;
    }

    undo_move(pos, m->pv[0]);
    if (!success) return 0;

    // Better moves are ranked higher. Guaranteed wins are ranked equally.
    // Losing moves are ranked equally unless a 50-move draw is in sight.
    // Note that moves ranked 900 have dtz + cnt50 == 100, which in rare
    // cases may be insufficient to win as dtz may be one off (see the
    // comments before TB_probe_dtz()).
    int r =  v > 0 ? (v + cnt50 <= 99 && !rep ? 1000 : 1000 - (v + cnt50))
           : v < 0 ? (-v * 2 + cnt50 < 100 ? -1000 : -1000 + (-v + cnt50))
           : 0;
    m->TBRank = r;

    // Determine the score to be displayed for this move. Assign at least
    // 1 cp to cursed wins and let it grow to 49 cp as the position gets
    // closer to a real win.
    m->TBScore =  r >= bound ? VALUE_MATE - MAX_PLY - 1
                : r >  0     ? max( 3, r - 800) * PawnValueEg / 200
                : r == 0     ? VALUE_DRAW
                : r > -bound ? min(-3, r + 800) * PawnValueEg / 200
                :             -VALUE_MATE + MAX_PLY + 1;
  }

  return 1;
}

// Use the WDL tables to rank all root moves in the list.
// This is a fallback for the case that some or all DTZ tables are missing.
// A return value of 0 means that not all probes were successful.
int TB_root_probe_wdl(Pos *pos, RootMoves *rm)
{
  static int wdl_to_rank[] = { -1000, -899, 0, 899, 1000 };
  static Value wdl_to_Value[] = {
    -VALUE_MATE + MAX_PLY + 1,
    VALUE_DRAW - 2,
    VALUE_DRAW,
    VALUE_DRAW + 2,
    VALUE_MATE - MAX_PLY - 1
  };

  int v, success;
  int move50 = option_value(OPT_SYZ_50_MOVE);

  // Probe, rank and score each move.
  pos->st->endMoves = (pos->st-1)->endMoves;
  for (int i = 0; i < rm->size; i++) {
    RootMove *m = &rm->move[i];
    do_move(pos, m->pv[0], gives_check(pos, pos->st, m->pv[0]));
    v = -TB_probe_wdl(pos, &success);
    undo_move(pos, m->pv[0]);
    if (!success) return 0;
    if (!move50)
      v = v > 0 ? 2 : v < 0 ? -2 : 0;
    m->TBRank = wdl_to_rank[v + 2];
    m->TBScore = wdl_to_Value[v + 2];
  }

  return 1;
}

// Use the DTM tables to find mate scores.
// Either DTZ or WDL must have been probed successfully earlier.
// A return value of 0 means that not all probes were successful.
int TB_root_probe_dtm(Pos *pos, RootMoves *rm)
{
  int success;
  Value tmpScore[rm->size];

  // Probe each move.
  pos->st->endMoves = (pos->st-1)->endMoves;
  for (int i = 0; i < rm->size; i++) {
    RootMove *m = &rm->move[i];

    // Use TBScore to find out if the position is won or lost.
    int wdl =  m->TBScore >  PawnValueEg ?  2
             : m->TBScore < -PawnValueEg ? -2 : 0;

    if (wdl == 0)
      tmpScore[i] = 0;
    else {
      // Probe and adjust mate score by 1 ply.
      do_move(pos, m->pv[0], gives_check(pos, pos->st, m->pv[0]));
      Value v = -TB_probe_dtm(pos, -wdl, &success);
      tmpScore[i] = wdl > 0 ? v - 1 : v + 1;
      undo_move(pos, m->pv[0]);
      if (success == 0)
        return 0;
    }
  }

  // All probes were successful. Now adjust TB scores and ranks.
  for (int i = 0; i < rm->size; i++) {
    RootMove *m = &rm->move[i];

    m->TBScore = tmpScore[i];

    // Let rank correspond to mate score, except for critical moves
    // ranked 900, which we rank below all other mates for safety.
    // By ranking mates above 1000 or below -1000, we let the search
    // know it need not search those moves.
    m->TBRank = m->TBRank == 900 ? 1001 : m->TBScore;
  }

  return 1;
}

// Use the DTM tables to complete a PV with mate score.
void TB_expand_mate(Pos *pos, RootMove *move)
{
  int success = 1, chk = 0;
  Value v = move->score, w = 0;
  int wdl = v > 0 ? 2 : -2;
  ExtMove *m;

  if (move->pv_size == MAX_PLY)
    return;

  // First get to the end of the incomplete PV.
  for (int i = 0; i < move->pv_size; i++) {
    v = v > 0 ? -v - 1 : -v + 1;
    wdl = -wdl;
    pos->st->endMoves = (pos->st-1)->endMoves;
    do_move(pos, move->pv[i], gives_check(pos, pos->st, move->pv[i]));
  }

  // Now try to expand until the actual mate.
  if (popcount(pieces()) <= TB_CardinalityDTM)
    while (v != -VALUE_MATE && move->pv_size < MAX_PLY) {
      v = v > 0 ? -v - 1 : -v + 1;
      wdl = -wdl;
      pos->st->endMoves = generate_legal(pos, (pos->st-1)->endMoves);
      for (m = (pos->st-1)->endMoves; m < pos->st->endMoves; m++) {
        do_move(pos, m->move, gives_check(pos, pos->st, m->move));
        if (wdl < 0)
          chk = TB_probe_wdl(pos, &success); // verify that m->move wins
        w =  success && (wdl > 0 || chk < 0)
           ? TB_probe_dtm(pos, wdl, &success)
           : 0;
        undo_move(pos, m->move);
        if (!success || v == w) break;
      }
      if (!success || v != w)
        break;
      move->pv[move->pv_size++] = m->move;
      do_move(pos, m->move, gives_check(pos, pos->st, m->move));
    }

  // Move back to the root position.
  for (int i = move->pv_size - 1; i >= 0; i--)
    undo_move(pos, move->pv[i]);
}

