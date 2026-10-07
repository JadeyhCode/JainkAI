/* tools/mdump.c — 把一个 model.bin 摊成文本，用来 diff 两个模型
 *
 * 编译：cc -std=c11 -O0 -g -o mdump tools/mdump.c -lm
 * 用法：./mdump a.bin > a.txt && ./mdump b.bin > b.txt && diff a.txt b.txt
 */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>

static void dump_ai(const AI *ai, FILE *o) {
    fprintf(o, "pool.count=%zu tick=%llu\n", ai->pool->count,
            (unsigned long long)ai->pool->current_tick);
    size_t na = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) na += ai->pool->tokens[i]->count;
    fprintf(o, "assocs=%zu n_learned=%zu turns=%zu\n", na, ai->n_learned, ai->turns);
    fprintf(o, "rng=%u gc_rng=%u energy=%.9g decay=%.9g cutoff=%.9g\n",
            ai->rng, ai->gc_rng, ai->energy, ai->decay, ai->cutoff);
    fprintf(o, "max_gen=%zu consolidate_every=%zu decay_factor=%.9g\n",
            ai->max_gen, ai->consolidate_every, ai->decay_factor);
    fprintf(o, "inner: pa=%.9g surp=%.9g comfort=%.9g disc=%.9g arous=%.9g fat=%.9g\n",
            ai->inner.predict_accuracy, ai->inner.surprise_level, ai->inner.comfort,
            ai->inner.discomfort, ai->inner.arousal, ai->inner.fatigue);
    fprintf(o, "meta: c=%.9g fam=%.9g flu=%.9g eff=%.9g gap=%.9g tot=%.9g\n",
            ai->meta.confidence, ai->meta.familiarity, ai->meta.fluency,
            ai->meta.effort, ai->meta.gap_feel, ai->meta.total);
    fprintf(o, "focus_n=%zu focus=%s last_expr_mode=%d last_conf=%.9g last_conf_out=%.9g\n",
            ai->focus_n, ai->focus ? ai->focus->name : "(null)", ai->last_expr_mode,
            ai->last_confidence, ai->last_confidence_at_output);
    fprintf(o, "trace_n=%zu last_output_n=%zu last_skipped_n=%zu last_path_n=%zu\n",
            ai->trace_n, ai->last_output_n, ai->last_skipped_n, ai->last_path.n);
    fprintf(o, "timeline_n=%zu head=%zu others_n=%zu\n",
            ai->timeline_n, ai->timeline_head, ai->others_n);
    fprintf(o, "counters: surprises=%zu abst=%zu sleeps=%zu gcdel_e=%zu gcdel_t=%zu "
               "credit=%zu exprl=%zu cf=%zu imag=%zu imag_u=%zu idlg=%zu narr=%zu "
               "drift=%zu diag=%zu\n",
            ai->n_surprises, ai->n_abstractions, ai->n_sleeps,
            ai->n_gc_deleted_edges, ai->n_gc_deleted_tokens, ai->n_credit_events,
            ai->n_expr_learned, ai->n_counterfactual, ai->n_imaginations,
            ai->n_imagine_useful, ai->n_inner_dialogues, ai->n_narrations,
            ai->n_drifts_detected, ai->n_diagnoses);
    fprintf(o, "calib: nc=%zu ncw=%zu bias=%.9g\n",
            ai->calib.n_confident, ai->calib.n_confident_wrong, ai->calib.calibration_bias);
    fprintf(o, "cfg: n_cand=%d sleep_every=%zu cursor=%zu target=%zu in_abst=%d\n",
            ai->n_candidates, ai->sleep_every, ai->sleep_cursor, ai->sleep_target,
            ai->sleep_in_abstraction);
    fprintf(o, "rates: imagine=%.9g idlg_gain=%.9g narrate_every=%zu drift_every=%zu\n",
            ai->imagine_rate, ai->inner_dialogue_gain, ai->narrate_every,
            ai->drift_sample_every);
    fprintf(o, "gc: min_age=%llu scale=%llu rate=%.9g interval=%zu del_state=%u\n",
            (unsigned long long)ai->gc.gc_min_age,
            (unsigned long long)ai->gc.gc_time_scale, ai->gc.gc_upgrade_rate,
            ai->gc.gc_interval, ai->gc.gc_delete_state);
    fprintf(o, "unc: %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
            ai->uncertain.w_gap, ai->uncertain.w_conflict, ai->uncertain.w_activation,
            ai->uncertain.w_effort, ai->uncertain.w_novelty,
            ai->uncertain.certain_threshold, ai->uncertain.unknown_threshold);
    fprintf(o, "setpoint: %.9g %.9g %.9g\n", ai->setpoint.setpoint_comfort,
            ai->setpoint.setpoint_arousal, ai->setpoint.regulation_rate);
    fprintf(o, "diag: ld=%.9g pn=%zu pa=%zu pi=%zu piu=%zu tick=%llu\n",
            ai->diag.prev_lexical_diversity, ai->diag.prev_perspective_n,
            ai->diag.prev_abstractions, ai->diag.prev_imaginations,
            ai->diag.prev_imagine_useful, (unsigned long long)ai->diag.last_diag_tick);
    for (int i = 0; i < EXPR_COUNT; ++i)
        fprintf(o, "expr[%d]=%s\n", i, ai->expr_root[i] ? ai->expr_root[i]->name : "(null)");
    for (size_t i = 0; i < ai->pool->count; ++i) {
        const Token *t = ai->pool->tokens[i];
        fprintf(o, "T%zu id=%u name=%s cat=%u garb=%u mem=%u miss=%u rmiss=%u "
                   "bt=%llu lt=%llu parent=%s next=%s pof=%s edges=%zu\n",
                i, t->id, t->name ? t->name : "(null)", t->is_category, t->is_garbage,
                t->mem_state, t->misses, t->recent_misses,
                (unsigned long long)t->birth_tick, (unsigned long long)t->last_touch_tick,
                t->parent ? t->parent->name : "-",
                t->next_in_time ? t->next_in_time->name : "-",
                t->perspective_of ? t->perspective_of->name : "-", t->count);
        for (size_t j = 0; j < t->count; ++j) {
            const AssocEntry *e = &t->assocs[j];
            fprintf(o, "  E%zu main=%s act=%s hits=%u seq=%u mem=%u bt=%llu lt=%llu sub=",
                    j, e->main ? (e->main->name ? e->main->name : "?") : "-",
                    e->action ? e->action->name : "-", e->hits, e->seq, e->mem_state,
                    (unsigned long long)e->birth_tick, (unsigned long long)e->last_touch_tick);
            for (uint8_t k = 0; k < e->n_subs; ++k)
                fprintf(o, "%s,", e->subs[k] && e->subs[k]->name ? e->subs[k]->name : "?");
            fprintf(o, " not=");
            for (uint8_t k = 0; k < e->n_not; ++k)
                fprintf(o, "%s,", e->subs_not[k] && e->subs_not[k]->name ? e->subs_not[k]->name : "?");
            fprintf(o, "\n");
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "用法: %s model.bin\n", argv[0]); return 2; }
    AI *ai = ai_load_file(argv[1]);
    if (!ai) { fprintf(stderr, "加载失败: %s\n", argv[1]); return 1; }
    FILE *o = (argc > 2) ? fopen(argv[2], "w") : stdout;
    if (!o) { perror("fopen"); ai_free(ai); return 1; }
    dump_ai(ai, o);
    if (o != stdout) fclose(o);
    ai_free(ai);
    return 0;
}
