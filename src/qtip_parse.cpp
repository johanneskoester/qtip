#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cassert>
#include <cctype>
#include <iostream>
#include <algorithm>
#include <string>
#include <vector>
#include <limits>
#include "ds.h"
#include "template.h"
#include "input_model.h"
#include "rnglib.hpp"
#include "simplesim.h"

using namespace std;

/**
 * Two kinds of output records.
 *
 * Input model templates:
 * ======================
 * 
 * Unpaired columns:
 * 1. Best score
 * 2. FW flag (T or F)
 * 3. Quality string
 * 4. Read length
 * 5. Mate flag (0, 1 or 2)
 * 6. Opposite mate read length
 * 7. Edit transcript
 *
 * Paired-end columns:
 * 1. Sum of best scores of both mates
 * 2. Mate 1 FW flag (T or F)
 * 3. Mate 1 quality string
 * 4. Mate 1 best score
 * 5. Mate 1 read length
 * 6. Mate 1 edit transcript
 * 7 .Mate 2 FW flag (T or F)
 * 8. Mate 2 quality string
 * 9. Mate 2 best score
 * 10. Mate 2 read length
 * 11. Mate 2 edit transcript
 * 12. Mate-1-upstream flag (T or F)
 * 13. Fragment length
 *
 * Feature records:
 * ===============
 *
 * Unpaired columns:
 * 1. Alignment id, so predictions can be ordered in parallel to input SAM
 * 2. Read length
 * 3. Reported MAPQ
 * 4. Template length
 * 5+. All the ZT:Z fields
 *
 * Paired-end columns:
 * 1. Alignment id, so predictions can be ordered in parallel to input SAM
 * 2. Mate 1 read length
 * 3. Mate 1 reported MAPQ
 * 4. Mate 2 read length
 * 5. Mate 2 reported MAPQ
 * 6. Fragment length
 * 7+. All the ZT:Z fields for mate 1
 * X+. All the ZT:Z fields for mate 2
 */

/**
 * Implementations of the various passes that qtip makes over SAM files.
 *
 * NOTE: uses strtok so not multithreaded.
 */

/* 64K buffer for all input and output */
const static size_t BUFSZ = 65536;

struct OpRunOffset {
	char op;
	int run;
	int offset;
	
	void init(char _op, int _run, int _offset) {
		op = _op;
		run = _run;
		offset = _offset;
	}
};

struct Alignment {
	
	Alignment() { clear(); }
	
	~Alignment() { }
	
	void clear() {
		rest_of_line = NULL;
		valid = false;
		qname = NULL;
		typ = NULL;
		flag = 0;
		rname = NULL;
		pos = 0;
		mapq = 0;
		cigar = NULL;
		rnext = NULL;
		pnext = 0;
		seq = NULL;
		len = 0;
		qual = NULL;
		avg_clipped_qual = 0.0;
		avg_aligned_qual = 0.0;
		mdz = NULL;
		cigar_equal_x = false;
		best_score = 0;
		left_clip = 0;
		right_clip = 0;
		rf_aln_buf.clear();
		rd_aln_buf.clear();
		edit_xscript.clear();
		cigar_ops.clear();
		cigar_run.clear();
		mdz_char.clear();
		mdz_oro.clear();
		correct = -1;
		line = 0;
	}
	
	inline bool is_aligned() const {
		return (flag & 4) == 0;
	}
	
	inline bool is_fw() const {
		return (flag & 16) == 0;
	}
	
	inline bool is_concordant() const {
		return (flag & 2) != 0;
	}
	
	inline bool is_paired() const {
		return (flag & 1) != 0;
	}
	
	inline char mate_flag() const {
		return ((flag & 64) != 0) ? '1' : (((flag & 128) != 0) ? '2' : '0');
	}

	/**
	 * Return fragment length, as inferred from pos & CIGAR.  Don't rely on
	 * tlen, where there's ambiguity about how to treat soft clipping.
	 */
	static size_t fragment_length(const Alignment& al1, const Alignment& al2) {
		const Alignment& up = (al1.pos < al2.pos) ? al1 : al2;
		const Alignment& dn = (al1.pos < al2.pos) ? al2 : al1;
		return dn.rpos() - up.lpos() + 1;
	}
	
	/**
	 * Return leftmost reference pos involved in the alignment, considering
	 * soft clipped bases as being included in the alignment.
	 */
	size_t lpos() const {
		assert(!cigar_ops.empty());
		return pos - left_clip;
	}
	
	/**
	 * Return rightmost reference pos involved in the alignment, considering
	 * soft clipped bases as being included in the alignment.
	 */
	size_t rpos() const {
		assert(!edit_xscript.empty());
		size_t mv = 0;
		const char *xs = edit_xscript.ptr();
		while(*xs++ == 'S');
		assert(*xs != '\0');
		while(*xs != '\0') {
			if(*xs == 'S' || *xs == 'D' || *xs == 'X' || *xs == '=') {
				mv++;
			}
			xs++;
		}
		return pos + mv - 1;
	}
	
	/**
	 * Return value is first pre-comma token of ZT:Z strings.s
	 */
	char * parse_extra(char *extra) {
		char *ztz = NULL;
		extra = strtok(extra, "\t");
		bool found_ztz = false, found_mdz = false;
		while(extra != NULL && (!found_mdz || !found_ztz)) {
			if(strncmp(extra, "ZT:Z:", 5) == 0) {
				ztz = extra + 5;
				found_ztz = true;
			}
			if(strncmp(extra, "MD:Z:", 5) == 0) {
				assert(mdz == NULL);
				mdz = extra + 5;
				mdz_to_list();
				found_mdz = true;
			}
			extra = strtok(NULL, "\t");
		}
		if(cigar != NULL && mdz != NULL && !cigar_equal_x) {
			cigar_and_mdz_to_edit_xscript();
		}
		if(ztz == NULL) {
			cerr << "Input SAM file did not have ZT:Z field.  Be sure to run"
			     << " a version of the aligner that produces the output needed"
			     << " for qtip." << endl;
			throw 1;
		}
		return ztz;
	}
	
	/**
	 * CIGAR string to list.
	 */
	void parse_cigar() {
		assert(cigar_ops.empty());
		assert(cigar_run.empty());
		assert(cigar != NULL);
		const size_t clen = strlen(cigar);
		int i = 0;
		while(i < clen) {
			assert(isdigit(cigar[i]));
			int run = 0;
			do {
				run *= 10;
				run += ((int)cigar[i] - (int)'0');
				i++;
			} while(isdigit(cigar[i]));
			assert(isalpha(cigar[i]) || cigar[i] == '=');
			if(cigar_ops.empty() && cigar[i] == 'S') {
				left_clip = run;
			} else if(i+1 >= clen && cigar[i] == 'S') {
				right_clip = run;
			}
			if(cigar[i] == 'X' || cigar[i] == '=') {
				cigar_equal_x = true;
			}
			cigar_ops.push_back(cigar[i]);
			cigar_run.push_back(run);
			i++;
		}
		if(cigar_equal_x) {
			cigar_to_edit_xscript();
		}
	}
	
	/**
	 * Parses the MD:Z string into the mdz_oro list.
	 */
	void mdz_to_list() {
		assert(mdz_char.empty());
		assert(mdz_oro.empty());
		assert(mdz != NULL);
		const size_t mlen = strlen(mdz);
		int i = 0;
		while(i < mlen) {
			if(isdigit(mdz[i])) {
				// Matching stretch
				int run = 0;
				do {
					run *= 10;
					run += ((int)mdz[i] - (int)'0');
					i++;
				} while(i < mlen && isdigit(mdz[i]));
				if(run > 0) {
					mdz_oro.expand();
					mdz_oro.back().init(0, run, -1);
				}
			} else if(isalpha(mdz[i])) {
				// Mismatching stretch
				int run = 0;
				do {
					mdz_char.push_back(mdz[i++]);
					run++;
				} while(i < mlen && isalpha(mdz[i]));
				mdz_oro.expand();
				mdz_oro.back().init(1, run, (int)(mdz_char.size() - run));
			} else if(mdz[i] == '^') {
				i++; // skip the ^
				int run = 0;
				while(i < mlen && isalpha(mdz[i])) {
					mdz_char.push_back(mdz[i++]);
					run++;
				}
				mdz_oro.expand();
				mdz_oro.back().init(2, run, (int)(mdz_char.size() - run));
			} else {
				fprintf(stderr, "Unexpected character at position %d of MD:Z string '%s'\n", i, mdz);
			}
		}
		assert(i == mlen);
	}

	/**
	 * Convert a CIGAR string with =s and Xs into an edit transcript.
	 *
	 * MODIFIES edit_xscript
	 */
	void cigar_to_edit_xscript() {
		assert(cigar_equal_x);
		assert(edit_xscript.empty());
		for(size_t i = 0; i < cigar_run.size(); i++) {
			char cop = cigar_ops[i];
			int crun = cigar_run[i];
			assert(cop != 'M' && cop != 'P');
			for(int j = 0; j < crun; j++) {
				edit_xscript.push_back(cop);
			}
		}
		edit_xscript.push_back(0);
	}

	/**
	 * Turn the CIGAR and MD:Z fields into an edit transcript.
	 *
	 * MODIFIES mdz_run
	 */
	void cigar_and_mdz_to_edit_xscript() {
		assert(!cigar_equal_x);
		assert(edit_xscript.empty());
		size_t mdo = 0;
		size_t rdoff = 0;
		for(size_t i = 0; i < cigar_run.size(); i++) {
			char cop = cigar_ops[i];
			int crun = cigar_run[i];
			assert(cop != 'X' && cop != '=');
			if(cop == 'M') {
				int mdrun = 0;
				int runleft = crun;
				while(runleft > 0 && mdo < mdz_oro.size()) {
					char op_m = mdz_oro[mdo].op;
					int run_m = mdz_oro[mdo].run;
					int run_comb = min<int>(runleft, run_m);
					runleft -= run_comb;
					assert(op_m == 0 || op_m == 1);
					if(op_m == 0) {
						for(size_t j = 0; j < run_comb; j++) {
							edit_xscript.push_back('=');
						}
					} else {
						assert(run_m == run_comb);
						for(size_t j = 0; j < run_m; j++) {
							edit_xscript.push_back('X');
						}
					}
					mdrun += run_comb;
					rdoff += run_comb;
					if(run_comb < run_m) {
						assert(op_m == 0);
						mdz_oro[mdo].run -= run_comb;
					} else {
						mdo++;
					}
				}
			} else if(cop == 'I') {
				for(size_t j = 0; j < crun; j++) {
					edit_xscript.push_back('I');
				}
				rdoff += crun;
			} else if(cop == 'D') {
				char op_m = mdz_oro[mdo].op;
				int run_m = mdz_oro[mdo].run;
				assert(op_m == 2);
				assert(crun == run_m);
				assert(run_m == crun);
				mdo++;
				for(size_t j = 0; j < run_m; j++) {
					edit_xscript.push_back('D');
				}
			} else if(cop == 'N') {
				for(size_t j = 0; j < crun; j++) {
					edit_xscript.push_back('N');
				}
			} else if(cop == 'S') {
				for(size_t j = 0; j < crun; j++) {
					edit_xscript.push_back('S');
				}
				rdoff += crun;
			} else if(cop == 'H') {
				// pass
			} else if(cop == 'P') {
				throw 1;
			} else if(cop == '=') {
				throw 1;
			} else if(cop == 'X') {
				throw 1;
			} else {
				throw 1;
			}
		}
		assert(mdo == mdz_oro.size());
		edit_xscript.push_back(0);
	}

	/**
	 * MODIFIES mdz_run
	 */
	void cigar_and_mdz_to_stacked() {
		size_t mdo = 0;
		size_t rdoff = 0;
		for(size_t i = 0; i < cigar_run.size(); i++) {
			char cop = cigar_ops[i];
			int crun = cigar_run[i];
			if(cop == 'M') {
				int mdrun = 0;
				int runleft = crun;
				while(runleft > 0 && mdo < mdz_oro.size()) {
					char op_m = mdz_oro[mdo].op;
					int run_m = mdz_oro[mdo].run;
					int st_m = mdz_oro[mdo].offset;
					int run_comb = min<int>(runleft, run_m);
					runleft -= run_comb;
					assert(op_m == 0 || op_m == 1);
					for(size_t j = rdoff; j < rdoff + run_comb; j++) {
						rd_aln_buf.push_back(seq[j]);
					}
					if(op_m == 0) {
						for(size_t j = rdoff; j < rdoff + run_comb; j++) {
							rf_aln_buf.push_back(seq[j]);
						}
					} else {
						assert(run_m == run_comb);
						for(size_t j = st_m; j < st_m + run_m; j++) {
							rf_aln_buf.push_back(mdz_char[j]);
						}
					}
					mdrun += run_comb;
					rdoff += run_comb;
					if(run_comb < run_m) {
						assert(op_m == 0);
						mdz_oro[mdo].run -= run_comb;
					} else {
						mdo++;
					}
				}
			} else if(cop == 'I') {
				for(size_t j = rdoff; j < rdoff + crun; j++) {
					rd_aln_buf.push_back(seq[j]);
					rf_aln_buf.push_back('-');
				}
				rdoff += crun;
			} else if(cop == 'D') {
				char op_m = mdz_oro[mdo].op;
				int run_m = mdz_oro[mdo].run;
				int st_m = mdz_oro[mdo].offset;
				assert(op_m == 2);
				assert(crun == run_m);
				assert(run_m == crun);
				mdo++;
				for(size_t j = st_m; j < st_m + run_m; j++) {
					rd_aln_buf.push_back('-');
					rf_aln_buf.push_back(mdz_char[j]);
				}
			} else if(cop == 'N') {
				for(size_t j = 0; j < crun; j++) {
					rd_aln_buf.push_back('-');
					rf_aln_buf.push_back('-');
				}
			} else if(cop == 'S') {
				rdoff += crun;
			} else if(cop == 'H') {
				// pass
			} else if(cop == 'P') {
				throw 1;
			} else if(cop == '=') {
				throw 1;
			} else if(cop == 'X') {
				throw 1;
			} else {
				throw 1;
			}
		}
		assert(mdo == mdz_oro.size());
		rd_aln_buf.push_back(0);
		rf_aln_buf.push_back(0);
	}

	/**
	 * Having already parsed the quality and CIGAR strings, we can now
	 * calculate the average quality of the aligned and clipped bases.
	 */
	void calc_qual_averages() {
		assert(len > 0);
		const size_t nclipped = left_clip + right_clip;
		assert(nclipped < len);
		tot_clipped_qual = 0;
		tot_aligned_qual = 0;
		for(size_t i = 0; i < len; i++) {
			assert((int)qual[i] >= 33);
			if(i < left_clip || i >= len - 1 - right_clip) {
				tot_clipped_qual += ((int)qual[i] - 33);
			} else {
				tot_aligned_qual += ((int)qual[i] - 33);
			}
		}
		avg_aligned_qual = (double)tot_aligned_qual / (len - nclipped);
		if(nclipped > 1) {
			avg_clipped_qual = (double)tot_clipped_qual / nclipped;
		} else {
			avg_clipped_qual = 100.0;
			tot_clipped_qual = 0;
			left_clip = right_clip = 0;
		}
	}
	
	/**
	 * If the read has a recognizable simulated read name, set the 'correct'
	 * field to 0/1 according to whether the alignment is correct.  Otherwise
	 * leave it as -1.
	 */
	void set_correctness(int wiggle) {
		assert(correct == -1);
		assert(is_aligned());
		const size_t rname_len = strlen(rname);
		if(strncmp(qname, sim_startswith, strlen(sim_startswith)) == 0) {
			correct = 0;
			// This is read simulated by qtip
			char *qname_cur = qname +strlen(sim_startswith);
			assert(*qname_cur == sim_sep);
			qname_cur++;
			if(mate_flag() != '2' &&
			   strncmp(qname_cur, rname, rname_len) != 0)
			{
				return;
			}
			qname_cur += rname_len;
			if(*(qname_cur++) != sim_sep) {
				return;
			}
			// now pointing to fw +/- flag
			if(mate_flag() != '2' && qname_cur[0] != (is_fw() ? '+' : '-')) {
				return;
			}
			qname_cur++;
			if(*(qname_cur++) != sim_sep) {
				return;
			}
			// now pointing to refoff
			size_t refoff = 0;
			while(isdigit(*qname_cur)) {
				refoff *= 10;
				refoff += (int)(*qname_cur++ - '0');
			}
			if(mate_flag() != '2' && abs((int)(refoff - (pos-1))) >= wiggle) {
				return;
			}
			if(*(qname_cur++) != sim_sep) {
				return;
			}
			// now pointing to score
			int score1 = 0;
			bool negative = false;
			if(*qname_cur == '-') {
				qname_cur++;
				negative = true;
			}
			while(isdigit(*qname_cur)) {
				score1 *= 10;
				score1 += (int)(*qname_cur++ - '0');
			}
			if(negative) {
				score1 = -score1;
			}
			if(*(qname_cur++) != sim_sep) {
				return;
			}
			if(qname_cur[0] == 'u' && (qname_cur[1] == '\0' || isspace(qname_cur[1]))) {
				correct = 1; // unpaired and correct
				return;
			}
			assert(mate_flag() != '0');
			if(mate_flag() != '2') {
				correct = 1; // paired-end, mate 1, and correct
				return;
			}
			// Final case: read is paired-end and mate 2
			if(strncmp(qname_cur, rname, rname_len) != 0)
			{
				return;
			}
			qname_cur += rname_len;
			if(*(qname_cur++) != sim_sep) {
				return;
			}
			// now pointing to fw +/- flag
			if(qname_cur[0] != (is_fw() ? '+' : '-')) {
				return;
			}
			qname_cur++;
			if(*(qname_cur++) != sim_sep) {
				return;
			}
			// now pointing to refoff
			refoff = 0;
			while(isdigit(*qname_cur)) {
				refoff *= 10;
				refoff += (int)(*qname_cur++ - '0');
			}
			if(abs((int)(refoff - (pos-1))) >= wiggle) {
				return;
			}
			if(*(qname_cur++) != sim_sep) {
				return;
			}
			// now pointing to score
			int score2 = 0;
			negative = false;
			if(*qname_cur == '-') {
				qname_cur++;
				negative = true;
			}
			while(isdigit(*qname_cur)) {
				score2 *= 10;
				score2 += (int)(*qname_cur++ - '0');
			}
			if(negative) {
				score2 = -score2;
			}
			if(*(qname_cur++) != sim_sep) {
				return;
			}
			assert(qname_cur[0] == 'b' || qname_cur[0] == 'c' || qname_cur[0] == 'd');
			correct = 1; // paired-end, mate 2, and correct
		} else {
			// This may still be a simulated read; depends on whether input
			// data was simulated.  Here we only check if it has a very special
			// read name format that's based on wgsim's.  That's the format
			// used by the qtip script for simulating input reads.
			
			// Example: 11_25006153_25006410_0:0:0_0:0:0_100_100_1_1/1
			//           ^ refid    ^ frag end (1-based) ^ len1  ^ flip
			//             ^ frag start (1-based)            ^ len2
			//                              (bunch of 0s)
			const size_t qname_len = strlen(qname);
			int nund = 0, ncolon = 0;
			for(size_t i = 0; i < qname_len; i++) {
				if(qname[i] == '_') {
					nund++;
				} else if(qname[i] == ':' && nund >= 3) {
					ncolon++;
				}
			}
			if(nund >= 8 && ncolon == 4) {
				char *qname_cur = qname;
				correct = 0;
				if(strncmp(qname_cur, rname, rname_len) != 0) {
					return;
				}
				qname_cur += rname_len; // skip over refid
				if(*qname_cur++ != '_') {
					return;
				}
				size_t frag_start = 0;
				while(isdigit(*qname_cur)) { // parse frag start
					frag_start *= 10;
					frag_start += (int)(*qname_cur++ - '0');
				}
				if(*qname_cur++ != '_') {
					return;
				}
				size_t frag_end = 0;
				while(isdigit(*qname_cur)) { // parse frag end
					frag_end *= 10;
					frag_end += (int)(*qname_cur++ - '0');
				}
				if(*qname_cur++ != '_') {
					return;
				}
				// skip over all the colons
				while(ncolon > 0) {
					if(*qname_cur++ == ':') {
						ncolon--;
					}
				}
				while(isdigit(*qname_cur++)); // skip number after last colon
				int len1 = 0;
				while(isdigit(*qname_cur)) {
					len1 *= 10;
					len1 += (int)(*qname_cur++ - '0');
				}
				if(*qname_cur++ != '_') {
					return;
				}
				int len2 = 0;
				while(isdigit(*qname_cur)) {
					len2 *= 10;
					len2 += (int)(*qname_cur++ - '0');
				}
				if(*qname_cur++ != '_') {
					return;
				}
				assert(*qname_cur == '0' || *qname_cur == '1');
				bool flip = (*qname_cur == '1');
				bool mate1 = mate_flag() != '2';
				int len = (mate1 ? len1 : len2);
				if(!flip == mate1) {
					// left end of fragment
					correct = abs((int)(pos - frag_start)) < wiggle;
				} else {
					// right end of fragment
					correct = abs((int)(pos - (frag_end - len + 1))) < wiggle;
				}
			}
		}
	}
	
	char *rest_of_line;
	bool valid;
	char *qname;
	char *typ;
	int flag;
	char *rname;
	size_t pos;
	int mapq;
	char *cigar;
	char *rnext;
	int pnext;
	char *seq;
	size_t len;
	char *qual;
	size_t tot_clipped_qual;
	size_t tot_aligned_qual;
	double avg_clipped_qual;
	double avg_aligned_qual;
	char *mdz;
	bool cigar_equal_x;
	int best_score;
	int left_clip;
	int right_clip;
	int correct;
	size_t line;
	
	// For holding stacked alignment result
	EList<char> rf_aln_buf;
	EList<char> rd_aln_buf;
	
	// For holding edit transcript
	EList<char> edit_xscript;

	// For holding cigar parsing info
	EList<int> cigar_run;
	EList<char> cigar_ops;
	
	// For holding MD:Z parsing info
	EList<OpRunOffset> mdz_oro;
	EList<char> mdz_char;
};

/**
 * strtok already used to parse up to rname.  Parse the rest and return char *
 * to the extra flags.
 *
 * This function uses strtok so it will interrrupt any strtoks already in
 * progress.  But when it returns, it's finished with its stateful strtok
 * session, so there's nothing keeping the caller from starting another
 * strtok.
 */
static char * parse_from_rname_on(Alignment& al) {
	assert(al.rest_of_line != NULL);
	al.rname = strtok(al.rest_of_line, "\t"); assert(al.rname != NULL);
	char *pos_str = strtok(NULL, "\t"); assert(pos_str != NULL);
	al.pos = (size_t)atoll(pos_str);
	char *mapq_str = strtok(NULL, "\t"); assert(mapq_str != NULL);
	al.mapq = atoi(mapq_str);
	assert(al.mapq < 256);

	// sets cigar_ops, cigar_run
	// if CIGAR string uses = and X, then also sets edit transcript
	al.cigar = strtok(NULL, "\t");
	assert(al.cigar != NULL);
	al.parse_cigar();

	al.rnext = strtok(NULL, "\t"); assert(al.rnext != NULL);
	char *pnext_str = strtok(NULL, "\t"); assert(pnext_str != NULL);
	al.pnext = atoi(pnext_str);
	strtok(NULL, "\t"); // ignore tlen
	al.seq = strtok(NULL, "\t"); assert(al.seq != NULL);
	al.len = strlen(al.seq);

	// sets qual, avg_aligned_qual and avg_clipped_qual
	al.qual = strtok(NULL, "\t");
	assert(al.qual != NULL);
	al.calc_qual_averages();

	al.rest_of_line = al.qual + strlen(al.qual) + 1;
	return al.rest_of_line;
}

int wiggle = 30;
int input_model_size = std::numeric_limits<int>::max();
float fraction_even = 1.0f;
float low_score_bias = 1.0f;
int max_allowed_fraglen = 50000;
float sim_factor = 30.0f;
int sim_function = FUNC_SQRT;
int sim_unp_min = 30000;
int sim_conc_min = 30000;
int sim_disc_min = 10000;
int sim_bad_end_min = 10000;

vector<double> write_buf;

/**
 * No guarantee about state of strtok upon return.
 */
static int print_unpaired(
	Alignment& al, // already parsed up through flags
	size_t ordlen,
	FILE *fh_model,
	FILE *fh_recs,
	ReservoirSampledEList<TemplateUnpaired> *unp_model)
{
	assert(al.is_aligned());
	char *extra = parse_from_rname_on(al);
	al.set_correctness(wiggle);
	char *ztz = al.parse_extra(extra);
	if(al.edit_xscript.empty()) {
		cerr << "Error: Input SAM file has neither extended CIGAR (using ="
		     << " and X instead of M) nor MD:Z field.  One or the other is"
		     << " required for use with qtip." << endl;
		throw 1;
	}
	char *ztz_tok = strtok(ztz, ",");
	assert(ztz_tok != NULL);
	al.best_score = atoi(ztz_tok);
	char fw_flag = al.is_fw() ? 'T' : 'F';
	
	if(fh_model != NULL) {
		// Output information relevant to input model
		fprintf(
			fh_model, "%d,%c,%s,%u,%c,%u,%s\n",
			al.best_score,
			fw_flag,
			al.qual,
			(unsigned)al.len,
			al.mate_flag(),
			(unsigned)ordlen,
			al.edit_xscript.ptr());
	}

	if(unp_model != NULL) {
		size_t off = unp_model->add_part1();
		if(off < unp_model->k()) {
			unp_model->list().back().init(
				al.best_score,
				(int)al.len,
				fw_flag,
				al.mate_flag(),
				(int)ordlen,
				al.qual,
				al.edit_xscript.ptr());
		}
	}
	
	if(fh_recs != NULL) {
		// Output information relevant to MAPQ model
		write_buf.push_back((double)al.line);
		write_buf.push_back((double)al.len);
		write_buf.push_back((double)(al.left_clip + al.right_clip));
		write_buf.push_back((double)al.tot_aligned_qual);
		write_buf.push_back((double)al.tot_clipped_qual);
		write_buf.push_back((double)ordlen);

		// ... including all the ZT:Z fields
		while(ztz_tok != NULL) {
			const char *buf = ztz_tok;
			if(*buf == 'N') {
				// Handle NA
				assert(*(++buf) == 'A');
				write_buf.push_back(std::numeric_limits<double>::quiet_NaN());
			} else {
				bool neg = false;
				bool added = false;
				int ztz_i = 0;
				if(*buf == '-') {
					neg = true;
					buf++;
				}
				while(*buf != '\0' && *buf != '\r' && *buf != '\n') {
					assert((*buf >= '0' && *buf <= '9') || *buf == '.');
					if(*buf != '.') {
						// Avoid sscanf if it's just an int
						ztz_i *= 10.0;
						ztz_i += ((*buf) - '0');
					} else {
						double ztz_d;
						sscanf(ztz_tok, "%lf", &ztz_d);
						write_buf.push_back(ztz_d);
						added = true;
						break;
					}
					buf++;
				}
				if(!added) {
					write_buf.push_back((double)(neg ? (-ztz_i) : ztz_i));
				}
			}
			ztz_tok = strtok(NULL, ",");
		}

		// ... and finish with MAPQ and correct
		write_buf.push_back((double)al.mapq);
		write_buf.push_back((double)al.correct);

		// Flush output buffer
		size_t nwritten = fwrite(&(write_buf.front()), 8,
			write_buf.size(), fh_recs);
		if(nwritten != write_buf.size()) {
			cerr << "Could not write all " << write_buf.size()
				 << " doubles to record file" << endl;
			return -1;
		}
		write_buf.clear();
	}
	return 0;
}

vector<double> ztz1_buf;
vector<double> ztz2_buf;

/**
 * No guarantee about state of strtok upon return.
 */
static int print_paired_helper(
	Alignment& al1,
	Alignment& al2,
	FILE *fh_model,
	FILE *fh_recs,
	ReservoirSampledEList<TemplatePaired> *paired_model)
{
	assert(al1.is_aligned());
	assert(al2.is_aligned());
	
	char *extra1 = parse_from_rname_on(al1);
	char *extra2 = parse_from_rname_on(al2);
	al1.set_correctness(wiggle);
	al2.set_correctness(wiggle);
	
	char *ztz1 = al1.parse_extra(extra1);
	if(al1.edit_xscript.empty()) {
		cerr << "Error: Input SAM file has neither extended CIGAR (using ="
		     << " and X instead of M) nor MD:Z field.  One or the other is"
		     << " required for use with qtip." << endl;
		throw 1;
	}
	char *ztz2 = al2.parse_extra(extra2);
	size_t fraglen = std::min((size_t)max_allowed_fraglen,
							  Alignment::fragment_length(al1, al2));
	bool upstream1 = al1.pos < al2.pos;
	assert(al1.cigar != NULL);
	assert(al2.cigar != NULL);

	char *ztz_tok1 = strtok(ztz1, ",");
	assert(ztz_tok1 != NULL);
	al1.best_score = atoi(ztz_tok1);
	char fw_flag1 = al1.is_fw() ? 'T' : 'F';	

    double len1_d;
    double clip1_d;
    double alqual1_d;
    double clipqual1_d;

	if(fh_recs != NULL) {
		ztz1_buf.clear();
		
		//
		// Mate 1
		//

		// Output information relevant to MAPQ model
		len1_d = (double)al1.len;
		clip1_d = (double)al1.left_clip + al1.right_clip;
		alqual1_d = (double)al1.tot_aligned_qual;
		clipqual1_d = (double)(double)al1.tot_clipped_qual;
		write_buf.push_back((double)al1.line);
		write_buf.push_back(len1_d);
		write_buf.push_back(clip1_d);
		write_buf.push_back(alqual1_d);
		write_buf.push_back(clipqual1_d);

		// ... including all the ZT:Z fields
		while(ztz_tok1 != NULL) {
			const char *buf = ztz_tok1;
			if(*buf == 'N') {
				// Handle NA
				assert(*(++buf) == 'A');
				write_buf.push_back(std::numeric_limits<double>::quiet_NaN());
				ztz1_buf.push_back(std::numeric_limits<double>::quiet_NaN());
			} else {
				bool neg = false;
				bool added = false;
				int ztz_i = 0;
				if(*buf == '-') {
					neg = true;
					buf++;
				}
				while(*buf != '\0' && *buf != '\r' && *buf != '\n') {
					assert((*buf >= '0' && *buf <= '9') || *buf == '.');
					if(*buf != '.') {
						// Avoid sscanf if it's just an int
						ztz_i *= 10.0;
						ztz_i += ((*buf) - '0');
					} else {
						double ztz_d;
						sscanf(ztz_tok1, "%lf", &ztz_d);
						write_buf.push_back(ztz_d);
						ztz1_buf.push_back(ztz_d);
						added = true;
						break;
					}
					buf++;
				}
				if(!added) {
					double ztz_add = (double)(neg ? (-ztz_i) : ztz_i);
					write_buf.push_back(ztz_add);
					ztz1_buf.push_back(ztz_add);
				}
			}
			ztz_tok1 = strtok(NULL, ",");
		}
	}
	
	char *ztz_tok2 = strtok(ztz2, ",");
	assert(ztz_tok2 != NULL);
	al2.best_score = atoi(ztz_tok2);
	char fw_flag2 = al2.is_fw() ? 'T' : 'F';
	
	if(fh_recs != NULL) {
		ztz2_buf.clear();
		
		//
		// Mate 2
		//

		// Output information relevant to MAPQ model
        const double len2_d = (double)al2.len;
        const double clip2_d = (double)(al2.left_clip + al2.right_clip);
        const double alqual2_d = (double)al2.tot_aligned_qual;
        const double clipqual2_d = (double)al2.tot_clipped_qual;
		const double fraglen_d = (double)fraglen;
		write_buf.push_back(len2_d);
		write_buf.push_back(clip2_d);
		write_buf.push_back(alqual2_d);
		write_buf.push_back(clipqual2_d);
		write_buf.push_back(fraglen_d);

		// ... including all the ZT:Z fields
		while(ztz_tok2 != NULL) {
			const char *buf = ztz_tok2;
			if(*buf == 'N') {
				// Handle NA
				assert(*(++buf) == 'A');
				write_buf.push_back(std::numeric_limits<double>::quiet_NaN());
				ztz2_buf.push_back(std::numeric_limits<double>::quiet_NaN());
			} else {
				bool neg = false;
				bool added = false;
				int ztz_i = 0;
				if(*buf == '-') {
					neg = true;
					buf++;
				}
				while(*buf != '\0' && *buf != '\r' && *buf != '\n') {
					assert((*buf >= '0' && *buf <= '9') || *buf == '.');
					if(*buf != '.') {
						// Avoid sscanf if it's just an int
						ztz_i *= 10.0;
						ztz_i += ((*buf) - '0');
					} else {
						double ztz_d;
						sscanf(ztz_tok2, "%lf", &ztz_d);
						write_buf.push_back(ztz_d);
						ztz2_buf.push_back(ztz_d);
						added = true;
						break;
					}
					buf++;
				}
				if(!added) {
					double ztz_add = (double)(neg ? (-ztz_i) : ztz_i);
					write_buf.push_back(ztz_add);
					ztz2_buf.push_back(ztz_add);
				}
			}
			ztz_tok2 = strtok(NULL, ",");
		}

		// ... and finish with MAPQ and correct
		write_buf.push_back((double)al1.mapq);
		write_buf.push_back((double)al1.correct);

		//
		// Now mate 2 again
		//
		write_buf.push_back((double)al2.line);
		write_buf.push_back(len2_d);
		write_buf.push_back(clip2_d);
		write_buf.push_back(alqual2_d);
		write_buf.push_back(clipqual2_d);
		write_buf.insert(write_buf.end(), ztz2_buf.begin(), ztz2_buf.end());
		write_buf.push_back(len1_d);
		write_buf.push_back(clip1_d);
		write_buf.push_back(alqual1_d);
		write_buf.push_back(clipqual1_d);
		write_buf.push_back(fraglen_d);
		write_buf.insert(write_buf.end(), ztz1_buf.begin(), ztz1_buf.end());
		write_buf.push_back((double)al2.mapq);
		write_buf.push_back((double)al2.correct);
		
		// Flush output buffer
		size_t nwritten = fwrite(&(write_buf.front()), 8,
			write_buf.size(), fh_recs);
		if(nwritten != write_buf.size()) {
			cerr << "Could not write all " << write_buf.size()
				 << " doubles to record file" << endl;
			return -1;
		}
		write_buf.clear();
	}

	if(fh_model != NULL) {
		// Output information relevant to input model
		fprintf(
			fh_model, "%d,%c,%s,%d,%u,%s,%c,%s,%d,%u,%s,%c,%llu\n",
			al1.best_score + al2.best_score,
			fw_flag1,
			al1.qual,
			al1.best_score,
			(unsigned)al1.len,
			al1.edit_xscript.ptr(),
			fw_flag2,
			al2.qual,
			al2.best_score,
			(unsigned)al2.len,
			al2.edit_xscript.ptr(),
			upstream1 ? 'T' : 'F',
			(unsigned long long)fraglen);
	}

	if(paired_model != NULL) {
		size_t j = paired_model->add_part1();
		if(j < paired_model->k()) {
			paired_model->list().back().init(
				al1.best_score + al2.best_score,
				al1.best_score,
				(int)al1.len,
				fw_flag1,
				al1.qual,
				al1.edit_xscript.ptr(),
				al2.best_score,
				(int)al2.len,
				fw_flag2,
				al2.qual,
				al2.edit_xscript.ptr(),
				upstream1,
				fraglen);
		}
	}
	return 0;
}

/**
 * Call print_paired_helper with the first alignment
 * (according to appearance in the SAM) first.
 */
static int print_paired(
	Alignment& al1,
	Alignment& al2,
	FILE *fh_model,
	FILE *fh_recs,
	ReservoirSampledEList<TemplatePaired> *paired_model)
{
	return print_paired_helper(al1.line < al2.line ? al1 : al2,
	                           al1.line < al2.line ? al2 : al1,
	                           fh_model,
	                           fh_recs,
	                           paired_model);
}

/**
 * Given a SAM record for an aligned read, count the number of comma-delimited
 * records in the ZT:Z extra field.
 */
static int infer_num_ztzs(const char *rest_of_line) {
	const char * cur = rest_of_line;
	int n_ztz_fields = 1;
	while(*cur != '\0') {
		if(*cur++ == '\t') {
			if(*cur++ == 'Z') {
				if(*cur++ == 'T') {
					if(strncmp(cur, ":Z:", 3) == 0) {
						while(*cur++ != '\n') {
							if(*cur == ',') {
								n_ztz_fields++;
							}
						}
					}
				}
			}
		}
	}
	return n_ztz_fields;
}

/**
 *
 */
static size_t infer_read_length(const char *rest_of_line) {
	size_t tabs = 0;
	size_t len = 0;
	const char *cur = rest_of_line;
	while(true) {
		if(*cur++ == '\t') {
			tabs++;
			if(tabs == 7) {
				while(*cur++ != '\t') {
					len++;
				}
				return len;
			}
		}
	}
	assert(false);
	return 0;
}

/**
 * Print column headers for an unpaired file of feature records.
 */
static void print_unpaired_header(FILE *fh, int n_ztz_fields, unsigned long long nrow) {
	fprintf(fh, "id,len,clip,alqual,clipqual,olen");
	for(int i = 0; i < n_ztz_fields; i++) {
		fprintf(fh, ",ztz%d", i);
	}
	fprintf(fh, ",mapq,correct,%llu\n", nrow);
}

/**
 * Print column headers for a paired-end file of feature records.
 */
static void print_paired_header(FILE *fh, int n_ztz_fields, unsigned long long nrow) {
	fprintf(fh, "id,len,clip,alqual,clipqual");
	for(int i = 0; i < n_ztz_fields; i++) {
		fprintf(fh, ",ztz_%d", i);
	}
	fprintf(fh, ",olen,oclip,oalqual,oclipqual,fraglen");
	for(int i = 0; i < n_ztz_fields; i++) {
		fprintf(fh, ",oztz_%d", i);
	}
	fprintf(fh, ",mapq,correct,%llu\n", nrow);
}

/**
 * Read the input SAM file while simultaneously writing out records used to
 * train a MAPQ model as well as records used to build an input model.
 */
static int sam_pass1(
	FILE *fh,
	const string& orec_u_fn, FILE *orec_u_fh,
	const string& orec_u_meta_fn, FILE *orec_u_meta_fh,
	const string& omod_u_fn, FILE *omod_u_fh,
	const string& orec_b_fn, FILE *orec_b_fh,
	const string& orec_b_meta_fn, FILE *orec_b_meta_fh,
	const string& omod_b_fn, FILE *omod_b_fh,
	const string& orec_c_fn, FILE *orec_c_fh,
	const string& orec_c_meta_fn, FILE *orec_c_meta_fh,
	const string& omod_c_fn, FILE *omod_c_fh,
	const string& orec_d_fn, FILE *orec_d_fh,
	const string& orec_d_meta_fn, FILE *orec_d_meta_fh,
	const string& omod_d_fn, FILE *omod_d_fh,
	ReservoirSampledEList<TemplateUnpaired> *u_templates,
	ReservoirSampledEList<TemplateUnpaired> *b_templates,
	ReservoirSampledEList<TemplatePaired> *c_templates,
	ReservoirSampledEList<TemplatePaired> *d_templates,
	bool quiet)
{
	/* Advise the kernel of our access pattern.  */
	/* posix_fadvise(fd, 0, 0, 1); */ /* FDADVICE_SEQUENTIAL */

    bool c_head = false, d_head = false, u_head = false, b_head = false;
    int c_nztz = 0, d_nztz = 0, u_nztz = 0, b_nztz = 0;

	char linebuf1[BUFSZ], linebuf2[BUFSZ];
	int line1 = 1;
	
	Alignment al1, al2;
	
	int al_cur1 = 1;
	
	int nline = 0, nhead = 0, nsec = 0, nsupp = 0, npair = 0, nunp = 0;
	int nunp_al = 0, nunp_unal = 0, npair_badend = 0, npair_conc = 0,
	    npair_disc = 0, npair_unal = 0, ntyp_mismatch = 0;
	
	while(1) {
		char *line = line1 ? linebuf1 : linebuf2;
		if(fgets(line, BUFSZ, fh) == NULL) {
			break; /* done */
		}
		nline++;
		if(line[0] == '@') {
			nhead++;
			continue; // skip header
		}
		char *qname = strtok(line, "\t"); assert(qname != NULL);
		assert(qname == line);
		char *flag_str = strtok(NULL, "\t"); assert(flag_str != NULL);
		int flag = atoi(flag_str);
		if((flag & 256) != 0) {
			nsec++;
			continue;
		}
		if((flag & 2048) != 0) {
			nsupp++;
			continue;
		}

		/* switch which buffer "line" points to */
		line1 = !line1;
		
		Alignment& al_cur  = al_cur1 ? al1 : al2;
		assert(!al_cur.valid);
		al_cur.clear();
		Alignment& al_prev = al_cur1 ? al2 : al1;
		al_cur1 = !al_cur1;
		
		al_cur.rest_of_line = flag_str + strlen(flag_str) + 1; /* for re-parsing */
		al_cur.qname = qname;
		al_cur.flag = flag;
		al_cur.line = nline;
		
		/* If we're able to mate up ends at this time, do it */
		Alignment *mate1 = NULL, *mate2 = NULL;
		if(al_cur.mate_flag() != '0' && al_prev.valid) {
			if(al_cur.mate_flag() == '1') {
				if(al_prev.mate_flag() != '2') {
					fprintf(stderr, "Consecutive records were both paired-end "
					                "but were not from opposite ends: "
					                "last_name=%s, name=%s\n",
					                al_prev.qname, al_cur.qname);
					throw 1;
				}
				assert(al_prev.mate_flag() == '2');
				mate1 = &al_cur;
				mate2 = &al_prev;
			} else {
				assert(al_cur.mate_flag() == '2');
				assert(al_prev.mate_flag() == '1');
				mate1 = &al_prev;
				mate2 = &al_cur;
			}
			mate1->valid = mate2->valid = false;
			npair++;
		}
		
		if(strncmp(al_cur.qname, sim_startswith, strlen(sim_startswith)) == 0) {
			// skip to final !
			char *cur = al_cur.qname;
			assert(al_cur.typ == NULL);
			while(*cur != '\0') {
				if(*cur == sim_sep) {
					al_cur.typ = cur+1;
				}
				cur++;
			}
			assert(al_cur.typ != NULL);
		}
		
		if(al_cur.mate_flag() == '0') {
			nunp++;
			
			// Case 1: Current read is unpaired and unaligned, we can safely skip
			if(!al_cur.is_aligned()) {
				nunp_unal++;
				continue;
			}
			
			// Case 2: Current read is unpaired and aligned
			else if(al_cur.typ == NULL || al_cur.typ[0] == 'u') {
				// If this is the first alignment, determine number of ZT:Z
				// fields and write a header line to the record output file
				if(nunp_al == 0 && orec_u_fh != NULL) {
                    u_head = true;
                    u_nztz = infer_num_ztzs(al_cur.rest_of_line);
				}

				nunp_al++;
				if(print_unpaired(al_cur, 0, omod_u_fh, orec_u_fh, u_templates) != 0) {
				    return -1;
				}
			}
			
			else if(al_cur.typ != NULL) {
				ntyp_mismatch++; // type mismatch
			}
		}
		
		else if(mate1 != NULL) {
			// Case 3: Current read is paired and unaligned, opposite mate is
			// also unaligned; nothing more to do!
			assert(mate2 != NULL);
			if(!mate1->is_aligned() && !mate2->is_aligned()) {
				npair_unal++;
				continue;
			}
			
			// Case 4: Current read is paired and aligned, opposite mate is unaligned
			// Case 5: Current read is paired and unaligned, opposite mate is aligned
			//         we handle both here
			else if(mate1->is_aligned() != mate2->is_aligned()) {
				bool m1al = mate1->is_aligned();
				Alignment& alm = m1al ? *mate1 : *mate2;
				if(alm.typ == NULL || (alm.typ[0] == 'b' && alm.typ[1] == alm.mate_flag())) {
					// If this is the first alignment, determine number of ZT:Z
					// fields and write a header line to the record output file
					if(npair_badend == 0 && orec_b_fh != NULL) {
                        b_head = true;
                        b_nztz = infer_num_ztzs(alm.rest_of_line);
					}

					npair_badend++;
					// the call to infer_read_length is needed because we
					// haven't parsed the sequence
					if(print_unpaired(
					    alm,
						infer_read_length(m1al ? mate2->rest_of_line : mate1->rest_of_line),
						omod_b_fh, orec_b_fh, b_templates) != 0)
					{
					    return -1;
					}
				}
				
				else if(alm.typ != NULL) {
					ntyp_mismatch++; // type mismatch
				}
			}
			
			else {
				assert(mate1->is_concordant() == mate2->is_concordant());
				
				if(mate1->is_concordant()) {
					if(mate1->typ == NULL || mate1->typ[0] == 'c') {
						if(npair_conc == 0 && orec_c_fh != NULL) {
                            c_head = true;
                            c_nztz = infer_num_ztzs(mate1->rest_of_line);
						}

						// Case 6: Current read is paired and both mates
						// aligned, concordantly
						npair_conc++;
						if(print_paired(*mate1, *mate2, omod_c_fh,
						                orec_c_fh, c_templates) != 0)
						{
						    return -1;
						}
					}
					
					else if(mate1->typ != NULL) {
						ntyp_mismatch++; // type mismatch
					}
				}
				
				else {
					if(mate1->typ == NULL || mate1->typ[0] == 'd') {
						if(npair_disc == 0 && orec_d_fh != NULL) {
                            d_head = true;
                            d_nztz = infer_num_ztzs(mate1->rest_of_line);
						}

						// Case 7: Current read is paired and both mates aligned, not condordantly
						npair_disc++;
						if(print_paired(*mate1, *mate2, omod_d_fh, orec_d_fh, d_templates) != 0) {
						    return -1;
						}
					}

					else if(mate1->typ != NULL) {
						ntyp_mismatch++; // type mismatch
					}
				}
			}
		}
		
		else {
			// This read is paired but we haven't seen the mate yet
			assert(al_cur.mate_flag() != '0');
			al_cur.valid = true;
		}
	}

    // Write metadata
    if(u_head) {
		print_unpaired_header(orec_u_meta_fh, u_nztz, nunp_al);
    }
    if(b_head) {
		print_unpaired_header(orec_b_meta_fh, b_nztz, npair_badend);
    }
    if(c_head) {
		print_paired_header(orec_c_meta_fh, c_nztz, npair_conc * 2);
    }
    if(d_head) {
		print_paired_header(orec_d_meta_fh, d_nztz, npair_disc * 2);
    }

	if(!quiet) {
		cerr << "  " << nline << " lines" << endl;
		cerr << "  " << nhead << " header lines" << endl;
		cerr << "  " << nsec << " secondary alignments ignored" << endl;
		cerr << "  " << nsupp << " supplementary alignments ignored" << endl;
		cerr << "  " << ntyp_mismatch << " alignment type didn't match simulated type" << endl;
		cerr << "  " << nunp << " unpaired" << endl;
		if(nunp > 0) {
			cerr << "    " << nunp_al << " aligned" << endl;
			cerr << "    " << nunp_unal << " unaligned" << endl;
		}
		cerr << "  " << npair << " paired-end" << endl;
		if(npair > 0) {
			cerr << "    " << npair_conc << " concordant" << endl;
			cerr << "    " << npair_disc << " discordant" << endl;
			cerr << "    " << npair_badend << " bad-end" << endl;
			cerr << "    " << npair_unal << " unaligned" << endl;
		}
	}
	
	return 0;
}

#define FILEDEC(fn, fh, buf, typ, do_open) \
	char buf [BUFSZ]; \
	FILE * fh = NULL; \
	if(do_open) { \
		fh = fopen( fn .c_str(), "wb"); \
		if(fh == NULL) { \
			cerr << "Could not open output " << typ << " file \"" << fn << "\"" << endl; \
			return -1; \
		} \
		setvbuf(fh, buf, _IOFBF, BUFSZ); \
	}

/**
 * Caller gives path to one or more SAM files, then the final argument is a prefix where all the
 */
int main(int argc, char **argv) {
	
	if(argc == 1) {
		// print which arguments from ts.py should pass through to here
		cout << "wiggle "
		     << "input-model-size "
		     << "fraction-even "
		     << "low-score-bias "
		     << "max-allowed-fraglen "
		     << "sim-factor "
		     << "sim-function "
		     << "sim-unp-min "
		     << "sim-conc-min "
		     << "sim-disc-min "
		     << "sim-bad-end-min "
		     << "seed "
		     << endl;
		return 0;
	}
	
	string orec_u_fn, omod_u_fn, oread_u_fn;
	string orec_b_fn, omod_b_fn, oread1_b_fn, oread2_b_fn;
	string orec_c_fn, omod_c_fn, oread1_c_fn, oread2_c_fn;
	string orec_d_fn, omod_d_fn, oread1_d_fn, oread2_d_fn;
    string orec_u_meta_fn;
    string orec_b_meta_fn;
    string orec_c_meta_fn;
    string orec_d_meta_fn;
	string prefix, mod_prefix;
	vector<string> fastas, sams;
	char buf_input_sam[BUFSZ];
	
	bool do_input_model = false; // output records related to input model
	bool do_simulation = false;  // do simulation
	bool do_features = false; // output records related to training/prediction
	bool keep_templates = false; // keep templates in memory for simulation
	assert(keep_templates || !do_simulation);
	int seed = 0;

	initialize(); // initialize ranlib

	// All arguments except last are SAM files to parse.  Final argument is
	// prefix for output files.
	int prefix_set = 0, mod_prefix_set = 0;
	{
		int section = 0;
		for(int i = 1; i < argc; i++) {
			if(strcmp(argv[i], "--") == 0) {
				section++;
				continue;
			}
			if(section == 0) {
				for(size_t j = 0; j < strlen(argv[i]); j++) {
					if(argv[i][j] == 's') {
						do_simulation = true;
					} else if(argv[i][j] == 'i') {
						do_input_model = true;
					} else if(argv[i][j] == 'f') {
						do_features = true;
					} else {
						cerr << "Warning: unrecognized option '" << argv[i][j]
						     << "'" << endl;
					}
				}
			} else if(section == 1) {
				if(i == argc-1) {
					cerr << "Error: odd number of arguments in options section" << endl;
					throw 1;
				}
				// this is where parameters get set
				else if(strcmp(argv[i], "wiggle") == 0) {
					wiggle = atoi(argv[++i]);
				}
				else if(strcmp(argv[i], "input-model-size") == 0) {
					input_model_size = atoi(argv[++i]);
				}
				else if(strcmp(argv[i], "fraction-even") == 0) {
					fraction_even = atof(argv[++i]);
					if(fraction_even < 1.0f) {
						cerr << "Warning: fraction-even not currently implemented" << endl;
					}
				}
				else if(strcmp(argv[i], "low-score-bias") == 0) {
					low_score_bias = atof(argv[++i]);
					if(low_score_bias < 1.0f) {
						cerr << "Warning: low-score bias not currently implemented" << endl;
					}
				}
				else if(strcmp(argv[i], "max-allowed-fraglen") == 0) {
					max_allowed_fraglen = atoi(argv[++i]);
				}
				else if(strcmp(argv[i], "sim-factor") == 0) {
					sim_factor = atof(argv[++i]);
				}
				else if(strcmp(argv[i], "sim-function") == 0) {
				    i++;
					if(strcmp(argv[i], "sqrt") == 0) {
					    sim_function = FUNC_SQRT;
					} else if(strcmp(argv[i], "linear") == 0) {
					    sim_function = FUNC_LINEAR;
					} else if(strcmp(argv[i], "const") == 0) {
					    sim_function = FUNC_CONST;
					} else {
						cerr << "Error: could not parse --sim-function argument: " << argv[i] << endl;
						return -1;
					}
				}
				else if(strcmp(argv[i], "sim-unp-min") == 0) {
					sim_unp_min = atoi(argv[++i]);
				}
				else if(strcmp(argv[i], "sim-conc-min") == 0) {
					sim_conc_min = atoi(argv[++i]);
				}
				else if(strcmp(argv[i], "sim-disc-min") == 0) {
					sim_disc_min = atoi(argv[++i]);
				}
				else if(strcmp(argv[i], "sim-bad-end-min") == 0) {
					sim_bad_end_min = atoi(argv[++i]);
				}
				else if(strcmp(argv[i], "seed") == 0) {
					// Unsure whether this is a good way to do this
					i++;
					seed = atoi(argv[i]);
					set_seed(seed, (atoi(argv[i])*77L) % 2147483562 + 1);
				}
			} else if(section == 2) {
				sams.push_back(string(argv[i]));
			} else if(section == 3) {
				fastas.push_back(string(argv[i]));
			} else if(section == 4) {
				prefix = argv[i];
				prefix_set++;
				
				// double matrices; input records for prediction
				orec_u_fn = prefix + string("_rec_u.npy");
				orec_b_fn = prefix + string("_rec_b.npy");
				orec_c_fn = prefix + string("_rec_c.npy");
				orec_d_fn = prefix + string("_rec_d.npy");

				// metadata for double matrices
				orec_u_meta_fn = prefix + string("_rec_u.meta");
				orec_b_meta_fn = prefix + string("_rec_b.meta");
				orec_c_meta_fn = prefix + string("_rec_c.meta");
				orec_d_meta_fn = prefix + string("_rec_d.meta");
			} else {
				mod_prefix = argv[i];
				mod_prefix_set++;

				// input models for simulation -- not actually written
				omod_u_fn = mod_prefix + string("_mod_u.csv");
				omod_b_fn = mod_prefix + string("_mod_b.csv");
				omod_c_fn = mod_prefix + string("_mod_c.csv");
				omod_d_fn = mod_prefix + string("_mod_d.csv");

				// simulated (tandem) reads
				oread_u_fn = mod_prefix + string("_reads_u.fastq");
				oread1_b_fn = mod_prefix + string("_reads_b_1.fastq");
				oread1_c_fn = mod_prefix + string("_reads_c_1.fastq");
				oread1_d_fn = mod_prefix + string("_reads_d_1.fastq");
				oread2_b_fn = mod_prefix + string("_reads_b_2.fastq");
				oread2_c_fn = mod_prefix + string("_reads_c_2.fastq");
				oread2_d_fn = mod_prefix + string("_reads_d_2.fastq");
			}
			if(prefix_set > 1) {
				cerr << "Warning: More than one output prefix specified; using last one: \"" << prefix << "\"" << endl;
			}
			if(mod_prefix_set > 1) {
				cerr << "Warning: More than one model output prefix specified; using last one: \"" << mod_prefix << "\"" << endl;
			}
		}
		if(sams.empty() || !prefix_set) {
			cerr << "Usage: qtip_parse_input [modes]* -- [argument value]* -- [sam]* -- [fasta]* -- [record prefix] -- [read/model prefix]" << endl;
			cerr << "[record prefix] is prefix for record files" << endl;
			cerr << "[read/model prefix] is prefix for simulated read and model files" << endl;
			cerr << "Modes:" << endl;
			cerr << "  f: write feature records for learning/prediction" << endl;
			cerr << "  i: write input-model templates (requires [read/model prefix])" << endl;
			cerr << "  s: simulate reads based on input model templates (requires [read/model prefix])" << endl;
			cerr << "Arguments:" << endl;
			// TODO: better documentation here
			cerr << "  wiggle <int>: if the reported alignment is within "
			     << "this many of the true alignment, it's considered correct"
			     << endl;
		}
	}
	keep_templates = do_simulation;

	if(do_simulation && mod_prefix_set == 0) {
		cerr << "s (simulation) argument specified, but [read/model prefix] not specified" << endl;
		return -1;
	}

	FILEDEC(orec_u_fn, orec_u_fh, orec_u_buf, "feature", do_features);
	FILEDEC(orec_u_meta_fn, orec_u_meta_fh, orec_u_meta_buf, "feature", do_features);
	FILEDEC(omod_u_fn, omod_u_fh, omod_u_buf, "template record", false);
	FILEDEC(orec_b_fn, orec_b_fh, orec_b_buf, "feature", do_features);
	FILEDEC(orec_b_meta_fn, orec_b_meta_fh, orec_b_meta_buf, "feature", do_features);
	FILEDEC(omod_b_fn, omod_b_fh, omod_b_buf, "template record", false);
	FILEDEC(orec_c_fn, orec_c_fh, orec_c_buf, "feature", do_features);
	FILEDEC(orec_c_meta_fn, orec_c_meta_fh, orec_c_meta_buf, "feature", do_features);
	FILEDEC(omod_c_fn, omod_c_fh, omod_c_buf, "template record", false);
	FILEDEC(orec_d_fn, orec_d_fh, orec_d_buf, "feature", do_features);
	FILEDEC(orec_d_meta_fn, orec_d_meta_fh, orec_d_meta_buf, "feature", do_features);
	FILEDEC(omod_d_fn, omod_d_fh, omod_d_buf, "template record", false);

	ReservoirSampledEList<TemplateUnpaired> u_templates(input_model_size);
	ReservoirSampledEList<TemplateUnpaired> b_templates(input_model_size);
	ReservoirSampledEList<TemplatePaired> c_templates(input_model_size);
	ReservoirSampledEList<TemplatePaired> d_templates(input_model_size);

	if(do_features || do_input_model || do_simulation) {
		for(size_t i = 0; i < sams.size(); i++) {
			cerr << "Parsing SAM file \"" << sams[i] << "\" (seed=" << seed << ")" << endl;
			FILE *fh = fopen(sams[i].c_str(), "rb");
			if(fh == NULL) {
				cerr << "Could not open input SAM file \"" << sams[i] << "\"" << endl;
				return -1;
			}
			setvbuf(fh, buf_input_sam, _IOFBF, BUFSZ);
			sam_pass1(fh,
					  orec_u_fn, orec_u_fh,
					  orec_u_meta_fn, orec_u_meta_fh,
					  omod_u_fn, omod_u_fh,
					  orec_b_fn, orec_b_fh,
					  orec_b_meta_fn, orec_b_meta_fh,
					  omod_b_fn, omod_b_fh,
					  orec_c_fn, orec_c_fh,
					  orec_c_meta_fn, orec_c_meta_fh,
					  omod_c_fn, omod_c_fh,
					  orec_d_fn, orec_d_fh,
					  orec_d_meta_fn, orec_d_meta_fh,
					  omod_d_fn, omod_d_fh,
					  keep_templates ? &u_templates : NULL,
					  keep_templates ? &b_templates : NULL,
					  keep_templates ? &c_templates : NULL,
					  keep_templates ? &d_templates : NULL,
					  false); // not quiet
			fclose(fh);
		}
	}

	if(omod_u_fh != NULL) fclose(omod_u_fh);
	if(orec_u_fh != NULL) fclose(orec_u_fh);
	if(orec_u_meta_fh != NULL) fclose(orec_u_meta_fh);
	if(omod_b_fh != NULL) fclose(omod_b_fh);
	if(orec_b_fh != NULL) fclose(orec_b_fh);
	if(orec_b_meta_fh != NULL) fclose(orec_b_meta_fh);
	if(omod_c_fh != NULL) fclose(omod_c_fh);
	if(orec_c_fh != NULL) fclose(orec_c_fh);
	if(orec_c_meta_fh != NULL) fclose(orec_c_meta_fh);
	if(omod_d_fh != NULL) fclose(omod_d_fh);
	if(orec_d_fh != NULL) fclose(orec_d_fh);
	if(orec_d_meta_fh != NULL) fclose(orec_d_meta_fh);
	cerr << "Finished parsing SAM" << endl;

	if(keep_templates) {
		cerr << "Input model in memory:" << endl;
		if(!u_templates.empty()) {
			cerr << "  Saved " << u_templates.list().size() << " unpaired templates "
			     << "(out of " << u_templates.size() << ")" << endl;
		}
		if(!b_templates.empty()) {
			cerr << "  Saved " << b_templates.list().size() << " bad-end templates "
			     << "(out of " << b_templates.size() << ")" << endl;
		}
		if(!c_templates.empty()) {
			cerr << "  Saved " << c_templates.list().size() << " concordant pair templates "
			     << "(out of " << c_templates.size() << ")" << endl;
		}
		if(!d_templates.empty()) {
			cerr << "  Saved " << d_templates.list().size() << " discordant pair templates "
			     << "(out of " << d_templates.size() << ")" << endl;
		}
	}

	if(do_simulation) {
		InputModelUnpaired u_model(u_templates.list(), u_templates.size(), fraction_even, low_score_bias);
		InputModelUnpaired b_model(b_templates.list(), b_templates.size(), fraction_even, low_score_bias);
		InputModelPaired c_model(c_templates.list(), c_templates.size(), fraction_even, low_score_bias);
		InputModelPaired d_model(d_templates.list(), d_templates.size(), fraction_even, low_score_bias);
		
		FILEDEC(oread_u_fn, oread_u_fh, oread_u_buf, "FASTQ", true);
		FILEDEC(oread1_b_fn, oread1_b_fh, oread1_b_buf, "FASTQ", true);
		FILEDEC(oread2_b_fn, oread2_b_fh, oread2_b_buf, "FASTQ", true);
		FILEDEC(oread1_c_fn, oread1_c_fh, oread1_c_buf, "FASTQ", true);
		FILEDEC(oread2_c_fn, oread2_c_fh, oread2_c_buf, "FASTQ", true);
		FILEDEC(oread1_d_fn, oread1_d_fh, oread1_d_buf, "FASTQ", true);
		FILEDEC(oread2_d_fn, oread2_d_fh, oread2_d_buf, "FASTQ", true);

		cerr << "Creating tandem read simulator" << endl;
		const size_t chunksz = 128 * 1024;
		StreamingSimulator ss(fastas, chunksz,
							  u_model, b_model, c_model, d_model,
							  oread_u_fh,
							  oread1_b_fh, oread2_b_fh,
							  oread1_c_fh, oread2_c_fh,
							  oread1_d_fh, oread2_d_fh);

		cerr << "  Estimate total number of FASTA bases is a bit less than "
		     << ss.num_estimated_bases() / 1000 << "k" << endl;
		
		cerr << "  Simulating reads..." << endl;
		ss.simulate_batch(
			sim_factor,
			sim_function,
			sim_unp_min,
			sim_conc_min,
			sim_disc_min,
			sim_bad_end_min);
		
		fclose(oread_u_fh);
		fclose(oread1_b_fh);
		fclose(oread2_b_fh);
		fclose(oread1_c_fh);
		fclose(oread2_c_fh);
		fclose(oread1_d_fh);
		fclose(oread2_d_fh);
	}
}
