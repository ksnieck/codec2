/*---------------------------------------------------------------------------*\

  FILE........: freedv_700.c
  AUTHOR......: David Rowe
  DATE CREATED: May 2020

  Functions that implement the various FreeDv 700 modes.

\*---------------------------------------------------------------------------*/

#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef TT
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__)
#include <sys/malloc.h>
#else
#include <malloc.h>
#endif /* __APPLE__ */
#endif

#include "fsk.h"
#include "fmfsk.h"
#include "codec2.h"
#include "codec2_fdmdv.h"
#include "varicode.h"
#include "freedv_api.h"
#include "freedv_api_internal.h"
#include "comp_prim.h"

#include "codec2_ofdm.h"
#include "ofdm_internal.h"
#include "mpdecode_core.h"
#include "gp_interleaver.h"
#include "interldpc.h"

extern char *ofdm_statemode[];

#ifdef __LPCNET__
void freedv_comptx_2020(struct freedv *f, COMP mod_out[]) {
    int    i, j, k;
    int    nspare;
 
    int data_bits_per_frame = f->ldpc->data_bits_per_frame;
    int bits_per_interleaved_frame = f->interleave_frames*data_bits_per_frame;
    uint8_t tx_bits[bits_per_interleaved_frame];

    memcpy(tx_bits, f->packed_codec_bits, bits_per_interleaved_frame);
    
    // Generate Varicode txt bits. Txt bits in OFDM frame come just
    // after Unique Word (UW).  Txt bits aren't protected by FEC, and need to be
    // added to each frame after interleaver as done it's thing

    nspare = f->ofdm_ntxtbits*f->interleave_frames;
    uint8_t txt_bits[nspare];

    for(k=0; k<nspare; k++) {
        if (f->nvaricode_bits == 0) {
            /* get new char and encode */
            char s[2];
            if (f->freedv_get_next_tx_char != NULL) {
                s[0] = (*f->freedv_get_next_tx_char)(f->callback_state);
                f->nvaricode_bits = varicode_encode(f->tx_varicode_bits, s, VARICODE_MAX_BITS, 1, 1);
                f->varicode_bit_index = 0;
            }
        }
        if (f->nvaricode_bits) {
            txt_bits[k] = f->tx_varicode_bits[f->varicode_bit_index++];
            f->nvaricode_bits--;
        }
    }

    /* optionally replace codec payload bits with test frames known to rx */

    if (f->test_frames) {
        uint8_t payload_data_bits[data_bits_per_frame];
        ofdm_generate_payload_data_bits(payload_data_bits, data_bits_per_frame);

        for (j=0; j<f->interleave_frames; j++) {
            for(i=0; i<data_bits_per_frame; i++) {
                tx_bits[j*data_bits_per_frame + i] = payload_data_bits[i];
            }
        }
    }

    /* OK now ready to LDPC encode, interleave, and OFDM modulate */
    
    complex float tx_sams[f->interleave_frames*f->n_nat_modem_samples];
    COMP asam;
    
    ofdm_ldpc_interleave_tx(f->ofdm, f->ldpc, tx_sams, tx_bits, txt_bits, f->interleave_frames, f->ofdm_config);

    for(i=0; i<f->interleave_frames*f->n_nat_modem_samples; i++) {
        asam.real = crealf(tx_sams[i]);
        asam.imag = cimagf(tx_sams[i]);
        mod_out[i] = fcmult(OFDM_AMP_SCALE*NORM_PWR_OFDM, asam);
    }

    if (f->clip) {
        cohpsk_clip(mod_out, OFDM_CLIP, f->interleave_frames*f->n_nat_modem_samples);
    }
}
#endif

#ifdef __LPCNET__
int freedv_comprx_2020(struct freedv *f, COMP demod_in[], int *valid) {
    int   i, j, nout, k;
    int   n_ascii;
    char  ascii_out;
    struct OFDM *ofdm = f->ofdm;
    struct LDPC *ldpc = f->ldpc;
    
    int    data_bits_per_frame = ldpc->data_bits_per_frame;
    int    coded_bits_per_frame = ldpc->coded_bits_per_frame;
    int    coded_syms_per_frame = ldpc->coded_syms_per_frame;
    int    interleave_frames = f->interleave_frames;
    COMP  *codeword_symbols = f->codeword_symbols;
    float *codeword_amps = f->codeword_amps;
    int    rx_bits[f->ofdm_bitsperframe];
    short txt_bits[f->ofdm_ntxtbits];
    COMP  payload_syms[coded_syms_per_frame];
    float payload_amps[coded_syms_per_frame];
   
    nout = 0;
    
    int Nerrs_raw = 0;
    int Nerrs_coded = 0;
    int iter = 0;
    int parityCheckCount = 0;
    uint8_t rx_uw[f->ofdm_nuwbits];

    /* echo samples back out as default (say if sync not found) */
    
    *valid = 1;
    f->sync = f->stats.sync = 0;
    
    /* TODO estimate this properly from signal */

    // TODO: should be higher for 2020?
    float EsNo = 3.0;
    
    /* looking for modem sync */
    
    if (ofdm->sync_state == search) {
        ofdm_sync_search(f->ofdm, demod_in);
    }

    /* OK modem is in sync */
    
    if ((ofdm->sync_state == synced) || (ofdm->sync_state == trial)) {
        ofdm_demod(ofdm, rx_bits, demod_in);
        ofdm_disassemble_modem_frame(ofdm, rx_uw, payload_syms, payload_amps, txt_bits);

        f->sync = 1;
        ofdm_get_demod_stats(f->ofdm, &f->stats);
        f->snr_est = f->stats.snr_est;
        
        assert((f->ofdm_nuwbits+f->ofdm_ntxtbits+coded_bits_per_frame) == f->ofdm_bitsperframe);

        /* now we need to buffer for de-interleaving -------------------------------------*/
                
        /* shift interleaved symbol buffers to make room for new symbols */
                
        for(i=0, j=coded_syms_per_frame; j<interleave_frames*coded_syms_per_frame; i++,j++) {
            codeword_symbols[i] = codeword_symbols[j];
            codeword_amps[i] = codeword_amps[j];
        }

        /* newest symbols at end of buffer (uses final i from last loop), note we 
           change COMP formats from what modem uses internally */
                
        for(i=(interleave_frames-1)*coded_syms_per_frame,j=0; i<interleave_frames*coded_syms_per_frame; i++,j++) {
            codeword_symbols[i] = payload_syms[j];
            codeword_amps[i]    = payload_amps[j];
         }
               
        /* run de-interleaver */
                
        COMP  codeword_symbols_de[interleave_frames*coded_syms_per_frame];
        float codeword_amps_de[interleave_frames*coded_syms_per_frame];
        gp_deinterleave_comp (codeword_symbols_de, codeword_symbols, interleave_frames*coded_syms_per_frame);
        gp_deinterleave_float(codeword_amps_de   , codeword_amps   , interleave_frames*coded_syms_per_frame);

        float llr[coded_bits_per_frame];
        uint8_t out_char[coded_bits_per_frame];

        interleaver_sync_state_machine(ofdm, ldpc, f->ofdm_config, codeword_symbols_de, codeword_amps_de, EsNo,
                                       interleave_frames, &iter, &parityCheckCount, &Nerrs_coded);
                                         
        if ((ofdm->sync_state_interleaver == synced) && (ofdm->frame_count_interleaver == interleave_frames)) {
            ofdm->frame_count_interleaver = 0;

            if (f->test_frames) {
                int tmp[interleave_frames];
                Nerrs_raw = count_uncoded_errors(ldpc, f->ofdm_config, tmp, interleave_frames, codeword_symbols_de);
                f->total_bit_errors += Nerrs_raw;
                f->total_bits       += f->ofdm_bitsperframe*interleave_frames;
            }

            f->modem_frame_count_rx = 0;
            
            for (j=0; j<interleave_frames; j++) {
                symbols_to_llrs(llr, &codeword_symbols_de[j * coded_syms_per_frame],
                                &codeword_amps_de[j * coded_syms_per_frame],
                                EsNo, ofdm->mean_amp, coded_syms_per_frame);                           
               if (ldpc->data_bits_per_frame == ldpc->ldpc_data_bits_per_frame) {
                    /* all data bits in code word used */
                    iter = run_ldpc_decoder(ldpc, out_char, llr, &parityCheckCount);
                } else {
                    /* some unused data bits, set these to known values to strengthen code */
                    float llr_full_codeword[ldpc->ldpc_coded_bits_per_frame];
                    int unused_data_bits = ldpc->ldpc_data_bits_per_frame - ldpc->data_bits_per_frame;
                                
                    // received data bits
                    for (i = 0; i < ldpc->data_bits_per_frame; i++)
                        llr_full_codeword[i] = llr[i]; 
                    // known bits ... so really likely
                    for (i = ldpc->data_bits_per_frame; i < ldpc->ldpc_data_bits_per_frame; i++)
                        llr_full_codeword[i] = -100.0;
                    // parity bits at end
                    for (i = ldpc->ldpc_data_bits_per_frame; i < ldpc->ldpc_coded_bits_per_frame; i++)
                        llr_full_codeword[i] = llr[i-unused_data_bits]; 
                    iter = run_ldpc_decoder(ldpc, out_char, llr_full_codeword, &parityCheckCount);
                }
                            
                if (f->test_frames) {
                    uint8_t payload_data_bits[data_bits_per_frame];
                    ofdm_generate_payload_data_bits(payload_data_bits, data_bits_per_frame);
                    Nerrs_coded = count_errors(payload_data_bits, out_char, data_bits_per_frame);
                    f->total_bit_errors_coded += Nerrs_coded;
                    f->total_bits_coded       += data_bits_per_frame;
                } else {
                    memcpy(f->packed_codec_bits+j*data_bits_per_frame, out_char, data_bits_per_frame);
                }
            } /* for interleave frames ... */
                   
            nout = f->n_speech_samples;                  
            *valid = 1;
            
        } /* if interleaver synced ..... */

        /* If modem is synced we can decode txt bits */
        
        for(k=0; k<f->ofdm_ntxtbits; k++)  { 
            //fprintf(stderr, "txt_bits[%d] = %d\n", k, rx_bits[i]);
            n_ascii = varicode_decode(&f->varicode_dec_states, &ascii_out, &txt_bits[k], 1, 1);
            if (n_ascii && (f->freedv_put_next_rx_char != NULL)) {
                (*f->freedv_put_next_rx_char)(f->callback_state, ascii_out);
            }
        }

        /* estimate uncoded BER from UW.  Coded bit errors could
           probably be estimated as half of all failed LDPC parity
           checks */

        for(i=0; i<f->ofdm_nuwbits; i++) {         
            if (rx_uw[i] != ofdm->tx_uw[i]) {
                f->total_bit_errors++;
            }
        }
        f->total_bits += f->ofdm_nuwbits;          

    }

    /* iterate state machine and update nin for next call */
    
    f->nin = ofdm_get_nin(ofdm);
    ofdm_sync_state_machine(ofdm, rx_uw);

    /* TODO: same sync logic as 700D - be useful to refactor/combine common code */
    bool sync = ((ofdm->sync_state == trial) && (parityCheckCount == ldpc->NumberParityBits)) || (ofdm->sync_state == synced);
    if (sync) {
        if (f->squelch_en && (f->snr_est < f->snr_squelch_thresh)) 
            *valid = 0; /* squelch if in sync but SNR too low */
        else {
            if (!f->squelch_en || (parityCheckCount == ldpc->NumberParityBits))
                *valid = 1; 
            else {
                /* let decoded audio through with reduced gain, as we may be end of over or in a fade */
                *valid = 2; 
            }
        }
    }
    else {
        /* pass through off air samples if squelch is disabled */
        if (f->squelch_en)
            *valid = 0;
        else {
            nout = f->nin;
            *valid = -1;
        }
    }

    if ((f->verbose && (ofdm->last_sync_state == search)) || (f->verbose == 2)) {
        fprintf(stderr, "%3d st: %-6s euw: %2d %1d f: %5.1f pbw: %d snr: %4.1f %2d eraw: %3d ecdd: %3d iter: %3d pcc: %3d vld: %d, nout: %4d\n",
                f->frames++, ofdm_statemode[ofdm->last_sync_state], ofdm->uw_errors, ofdm->sync_counter, 
		(double)ofdm->foff_est_hz, ofdm->phase_est_bandwidth,
                f->snr_est, ofdm->frame_count_interleaver,
                Nerrs_raw, Nerrs_coded, iter, parityCheckCount, *valid, nout);
    }
        
    return nout;
}
#endif