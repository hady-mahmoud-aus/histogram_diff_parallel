#include "histogram_output.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>

typedef struct { size_t start, end; } Group;

typedef struct {
    const FileData *file;
    const size_t *ids;
    unsigned char *changed;
    size_t count;
} Side;

static void group_init(const Side *s, Group *g) {
    g->start = g->end = 0;
    while (g->end < s->count && s->changed[g->end]) ++g->end;
}
static int group_next(const Side *s, Group *g) {
    if (g->end == s->count) return -1;
    g->start = ++g->end;
    while (g->end < s->count && s->changed[g->end]) ++g->end;
    return 0;
}
static int group_previous(const Side *s, Group *g) {
    if (g->start == 0) return -1;
    g->end = g->start - 1;
    g->start = g->end;
    while (g->start && s->changed[g->start - 1]) --g->start;
    return 0;
}
static int slide_up(Side *s, Group *g) {
    if (!g->start || s->ids[g->start - 1] != s->ids[g->end - 1]) return -1;
    s->changed[--g->start] = 1;
    s->changed[--g->end] = 0;
    while (g->start && s->changed[g->start - 1]) --g->start;
    return 0;
}
static int slide_down(Side *s, Group *g) {
    if (g->end == s->count || s->ids[g->start] != s->ids[g->end]) return -1;
    s->changed[g->start++] = 0;
    s->changed[g->end++] = 1;
    while (g->end < s->count && s->changed[g->end]) ++g->end;
    return 0;
}

/* Git 2.47.3 change-group compaction, without the post-2025 histogram
 * re-diff introduced by upstream commit e417277ae9 in March 2026.
 * That newer behavior changes repeated-line hunk placement for this oracle.
 * The caller disables Git's optional indent heuristic for comparisons. */
static int compact_side(Side *s, Side *other) {
    Group g, go;
    group_init(s, &g);
    group_init(other, &go);
    for (;;) {
        if (g.start != g.end) {
            size_t earliest_end, matching_end = SIZE_MAX, group_size;
            do {
                group_size = g.end - g.start;
                matching_end = SIZE_MAX;
                while (slide_up(s, &g) == 0) {
                    if (group_previous(other, &go)) return -1;
                }
                earliest_end = g.end;
                if (go.start != go.end) matching_end = g.end;
                while (slide_down(s, &g) == 0) {
                    if (group_next(other, &go)) return -1;
                    if (go.start != go.end) matching_end = g.end;
                }
            } while (group_size != g.end - g.start);
            if (g.end != earliest_end && matching_end != SIZE_MAX) {
                while (go.start == go.end) {
                    if (slide_up(s, &g) || group_previous(other, &go)) return -1;
                }
            }

        }
        if (group_next(s, &g)) break;
        if (group_next(other, &go)) return -1;
    }
    return group_next(other, &go) == -1 ? 0 : -1;
}

int histogram_compact(const FileData *a, const FileData *b,
                      const LineClasses *classes, HistogramChangeFlags *flags) {
    HistogramChangeFlags tmp = {0};
    Side old_side, new_side;
    if (!a || !b || !classes || !flags ||
        a->line_count != flags->old_count || b->line_count != flags->new_count ||
        (a->line_count && (!classes->old_ids || !flags->old_changed)) ||
        (b->line_count && (!classes->new_ids || !flags->new_changed))) return -1;
    for (size_t i=0;i<a->line_count;i++)
        if (classes->old_ids[i]>=classes->class_count) return -1;
    for (size_t i=0;i<b->line_count;i++)
        if (classes->new_ids[i]>=classes->class_count) return -1;
    if (histogram_change_flags_init(&tmp,a->line_count,b->line_count)) return -1;
    if (a->line_count) memcpy(tmp.old_changed,flags->old_changed,a->line_count);
    if (b->line_count) memcpy(tmp.new_changed,flags->new_changed,b->line_count);
    old_side=(Side){a,classes->old_ids,tmp.old_changed,a->line_count};
    new_side=(Side){b,classes->new_ids,tmp.new_changed,b->line_count};
    if (compact_side(&old_side,&new_side) ||
        compact_side(&new_side,&old_side)) {
        histogram_change_flags_free(&tmp); return -1;
    }
    if (a->line_count) memcpy(flags->old_changed,tmp.old_changed,a->line_count);
    if (b->line_count) memcpy(flags->new_changed,tmp.new_changed,b->line_count);
    histogram_change_flags_free(&tmp);
    return 0;
}

void histogram_script_free(HistogramEditScript *script) {
    if (script) { free(script->edits); *script=(HistogramEditScript){0}; }
}
int histogram_build_script(const HistogramChangeFlags *f, HistogramEditScript *out) {
    size_t i=0,j=0,n=0,cap=0;
    HistogramEdit *edits=NULL;
    if (!f || !out || (f->old_count && !f->old_changed) ||
        (f->new_count && !f->new_changed)) return -1;
    *out=(HistogramEditScript){0};
    while (i<f->old_count || j<f->new_count) {
        if ((i<f->old_count && f->old_changed[i]) ||
            (j<f->new_count && f->new_changed[j])) {
            size_t a=i,b=j;
            while(i<f->old_count && f->old_changed[i]) ++i;
            while(j<f->new_count && f->new_changed[j]) ++j;
            if (n==cap) {
                size_t newcap=cap ? cap*2 : 16;
                if (newcap<cap || newcap>SIZE_MAX/sizeof *edits) goto fail;
                HistogramEdit *grown=realloc(edits,newcap*sizeof *edits);
                if (!grown) goto fail;
                edits=grown;cap=newcap;
            }
            edits[n++]=(HistogramEdit){a,i-a,b,j-b};
        } else {
            if (i>=f->old_count || j>=f->new_count) goto fail;
            ++i;++j;
        }
    }
    out->edits=edits;out->count=n;return 0;
fail:
    free(edits);return -1;
}

static int emit_line(FILE *out, char marker, const FileData *f, size_t idx) {
    const Line *line=&f->lines[idx];
    if (fputc(marker,out)==EOF ||
        (line->length && fwrite(f->bytes+line->offset,1,line->length,out)!=line->length))
        return -1;
    if (line->ending==LINE_END_CRLF && fputc('\r',out)==EOF) return -1;
    if (fputc('\n',out)==EOF) return -1;
    if (line->ending==LINE_END_NONE &&
        fputs("\\ No newline at end of file\n",out)==EOF) return -1;
    return 0;
}
static int header(FILE *out,const FileData *old_file,size_t old_start,size_t old_len,
                  size_t new_start,size_t new_len) {
    size_t old_num=old_start+(old_len!=0),new_num=new_start+(new_len!=0);
    if (fprintf(out,"@@ -%zu",old_num)<0) return -1;
    if (old_len!=1 && fprintf(out,",%zu",old_len)<0) return -1;
    if (fprintf(out," +%zu",new_num)<0) return -1;
    if (new_len!=1 && fprintf(out,",%zu",new_len)<0) return -1;
    if (fputs(" @@",out)==EOF) return -1;
    /* Git's patch wrapper adds a trailing function/context label, based on
     * the nearest qualifying original line preceding the hunk. */
    for (size_t i=old_start;i>0;) {
        --i;
        const Line *ln=&old_file->lines[i];
        if (!ln->length) continue;
        const unsigned char *p=old_file->bytes+ln->offset;
        size_t n=ln->length;
        /* Match xemit.c's default def_ff() (not an arbitrary nonblank line). */
        if (!(isalpha((unsigned char)p[0]) || p[0]=='_' || p[0]=='$')) continue;
        if (n>80) n=80;
        while (n && isspace((unsigned char)p[n-1])) --n;
        if (fputc(' ',out)==EOF || fwrite(p,1,n,out)!=n) return -1;
        break;
    }
    return fputc('\n',out)==EOF ? -1:0;
}

int histogram_emit_unified(FILE *out,const FileData *a,const FileData *b,
                           const HistogramEditScript *s) {
    size_t k=0,old_cursor=0,new_cursor=0;
    if (!out || !a || !b || !s || (s->count && !s->edits)) return -1;
    while (k<s->count) {
        size_t first=k,last=k;
        const HistogramEdit *e=&s->edits[k];
        size_t os=e->old_start>3?e->old_start-3:0;
        size_t ns=e->new_start>3?e->new_start-3:0;
        size_t oe=e->old_start+e->old_length;
        size_t ne=e->new_start+e->new_length;
        if (oe>a->line_count || ne>b->line_count || os<old_cursor || ns<new_cursor) return -1;
        /* Merge changes whose three-line context regions overlap or touch. */
        while (last+1<s->count) {
            const HistogramEdit *next=&s->edits[last+1];
            if (next->old_start<oe || next->new_start<ne) return -1;
            if (next->old_start-oe>6 || next->new_start-ne>6) break;
            ++last;
            oe=next->old_start+next->old_length;
            ne=next->new_start+next->new_length;
        }
        oe=oe+3<a->line_count?oe+3:a->line_count;
        ne=ne+3<b->line_count?ne+3:b->line_count;
        if (header(out,a,os,oe-os,ns,ne-ns)) return -1;
        size_t i=os,j=ns;
        for (size_t p=first;p<=last;p++) {
            const HistogramEdit *x=&s->edits[p];
            if (x->old_start<i || x->new_start<j ||
                x->old_start-i!=x->new_start-j) return -1;
            while(i<x->old_start) {
                if (emit_line(out,' ',a,i++)) return -1;
                ++j;
            }
            for (size_t z=0;z<x->old_length;z++)
                if (emit_line(out,'-',a,i++)) return -1;
            for (size_t z=0;z<x->new_length;z++)
                if (emit_line(out,'+',b,j++)) return -1;
        }
        if (oe-i!=ne-j) return -1;
        while(i<oe) {
            if (emit_line(out,' ',a,i++)) return -1;
            ++j;
        }
        old_cursor=oe;new_cursor=ne;k=last+1;
    }
    return ferror(out)?-1:0;
}
