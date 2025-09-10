// DVGB19 Lab 2 — Virtual Memory Simulator (vmsim)
// Karlstad University — Implements FIFO, LRU, Optimal with pure demand paging
// Author: ChatGPT (assistant)
// Build (Linux/macOS/WSL):   gcc -O2 -std=c11 -Wall -Wextra -o vmsim vmsim.c
// Build (Windows/MinGW):     gcc -O2 -std=c11 -Wall -Wextra -o vmsim.exe vmsim.c
// Usage:
//   vmsim -a <fifo|lru|optimal> -n <frames> -f <trace file>
//
// Spec highlights:
//  • Virtual address space: 16-bit (0x0000–0xFFFF)
//  • Page/frame size: 256 bytes (thus 256 virtual pages total)
//  • Physical memory size: <frames> × 256 bytes; frames > 0
//  • Input trace: one hex address per line, e.g., 0x01FF
//  • For each access: print address, hit/fault, and any replacement (page out/in)
//  • Summary at the end: frames, total accesses, hits, faults, replacements
//
// The simulator preloads the entire trace to support OPT (Belady) efficiently.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stdbool.h>

#define PAGE_SIZE 256               // bytes per page/frame
#define VIRTUAL_PAGES 256           // 64 KiB / 256 B
#define INF_NEXT 0x7fffffff

// Replacement algorithms
typedef enum { ALG_FIFO, ALG_LRU, ALG_OPTIMAL } alg_t;

static const char* alg_name(alg_t a){
    switch(a){
        case ALG_FIFO: return "FIFO";
        case ALG_LRU: return "LRU";
        case ALG_OPTIMAL: return "Optimal";
        default: return "?";
    }
}

// Dynamic vector for trace of page numbers
typedef struct {
    int *data;        // page numbers (0..255)
    int  size;
    int  cap;
} ivec_t;

static void ivec_init(ivec_t *v){ v->data=NULL; v->size=0; v->cap=0; }
static void ivec_push(ivec_t *v, int x){
    if(v->size==v->cap){ v->cap = v->cap? v->cap*2 : 1024; v->data = (int*)realloc(v->data, v->cap*sizeof(int)); if(!v->data){ perror("realloc"); exit(1);} }
    v->data[v->size++] = x;
}
static void ivec_free(ivec_t *v){ free(v->data); v->data=NULL; v->size=v->cap=0; }

// For OPT: store future positions for each page in a vector and a pointer index
typedef struct { ivec_t pos; int ptr; } future_list_t;

// Simple line reader (robust to CRLF, blanks, comments)
static bool read_hex_address(FILE *fp, uint16_t *out){
    char buf[128];
    while(fgets(buf, sizeof(buf), fp)){
        // Trim leading spaces
        char *p = buf; while(isspace((unsigned char)*p)) p++;
        if(*p=='\0' || *p=='#' || *p=='\n') continue; // skip blanks/comments
        // Accept forms: 0xABCD, ABCD, 0xabcd
        unsigned int val;
        if(strncmp(p, "0x", 2)==0 || strncmp(p, "0X", 2)==0){
            if(sscanf(p, "%x", &val)!=1) continue;
        } else {
            if(sscanf(p, "%x", &val)!=1) continue;
        }
        *out = (uint16_t)(val & 0xFFFF);
        return true;
    }
    return false;
}

// Simulation state
typedef struct {
    alg_t alg;
    int frames;                        // number of physical frames (>0)
    int frame_pages_cap;               // == frames
    int *frame_page;                   // frame -> page (or -1 if free)
    int *page_to_frame;                // page -> frame (or -1 if not present)
    int next_fifo;                     // for FIFO round-robin index
    int *lru_age;                      // per frame: last used timestamp
    int time;                          // logical time for LRU

    // Stats
    long total_accesses;
    long hits;
    long faults;
    long replacements;
} sim_t;

static void sim_init(sim_t *s, alg_t alg, int frames){
    s->alg = alg;
    s->frames = frames;
    s->frame_pages_cap = frames;
    s->frame_page = (int*)malloc(sizeof(int)*frames);
    s->page_to_frame = (int*)malloc(sizeof(int)*VIRTUAL_PAGES);
    s->lru_age = (int*)malloc(sizeof(int)*frames);
    if(!s->frame_page || !s->page_to_frame || !s->lru_age){ perror("malloc"); exit(1);} 
    for(int f=0; f<frames; ++f){ s->frame_page[f] = -1; s->lru_age[f]=0; }
    for(int p=0; p<VIRTUAL_PAGES; ++p){ s->page_to_frame[p] = -1; }
    s->next_fifo = 0;
    s->time = 0;
    s->total_accesses = s->hits = s->faults = s->replacements = 0;
}

static void sim_free(sim_t *s){
    free(s->frame_page); s->frame_page=NULL;
    free(s->page_to_frame); s->page_to_frame=NULL;
    free(s->lru_age); s->lru_age=NULL;
}

static int find_free_frame(sim_t *s){
    for(int f=0; f<s->frames; ++f) if(s->frame_page[f]==-1) return f;
    return -1;
}

static int choose_victim_fifo(sim_t *s){
    int v = s->next_fifo; s->next_fifo = (s->next_fifo + 1) % s->frames; return v;
}

static int choose_victim_lru(sim_t *s){
    // Evict the frame with the smallest last-used timestamp
    int victim = 0; int best_age = s->lru_age[0];
    for(int f=1; f<s->frames; ++f){ if(s->lru_age[f] < best_age){ best_age = s->lru_age[f]; victim = f; } }
    return victim;
}

static int choose_victim_optimal(sim_t *s, future_list_t future[VIRTUAL_PAGES]){
    // Evict the page whose next use is farthest in the future (or never used again)
    int victim_f = 0; int victim_next = -1; // -1 means never used again (best)
    for(int f=0; f<s->frames; ++f){
        int p = s->frame_page[f];
        int nextpos;
        if(future[p].ptr < future[p].pos.size){
            nextpos = future[p].pos.data[ future[p].ptr ];
        } else {
            nextpos = INF_NEXT; // never used again
        }
        if(nextpos==INF_NEXT){ return f; } // perfect victim
        if(nextpos > victim_next){ victim_next = nextpos; victim_f = f; }
    }
    return victim_f;
}

static void print_hit(uint16_t addr, int page, int frame){
    printf("Access 0x%04X (page %3d): HIT  -> frame %d\n", addr, page, frame);
}

static void print_fault_loaded(uint16_t addr, int page, int frame){
    printf("Access 0x%04X (page %3d): FAULT -> page in -> frame %d\n", addr, page, frame);
}

static void print_fault_replaced(uint16_t addr, int page_in, int victim_page, int victim_frame){
    printf("Access 0x%04X (page %3d): FAULT -> REPLACE: page %d out (frame %d), page %d in\n",
           addr, page_in, victim_page, victim_frame, page_in);
}

static void simulate(sim_t *s, const ivec_t *trace_pages, const ivec_t *trace_addrs, future_list_t future[VIRTUAL_PAGES]){
    for(int i=0; i<trace_pages->size; ++i){
        int page = trace_pages->data[i];
        uint16_t addr = (uint16_t)trace_addrs->data[i];
        s->total_accesses++;
        s->time++;

        int frame = s->page_to_frame[page];
        if(frame != -1){
            // HIT
            s->hits++;
            if(s->alg == ALG_LRU){ s->lru_age[frame] = s->time; }
            print_hit(addr, page, frame);
        } else {
            // FAULT
            s->faults++;
            int freef = find_free_frame(s);
            if(freef != -1){
                // Load into a free frame
                s->frame_page[freef] = page;
                s->page_to_frame[page] = freef;
                if(s->alg == ALG_LRU){ s->lru_age[freef] = s->time; }
                print_fault_loaded(addr, page, freef);
            } else {
                // Need replacement
                int victim_f;
                if(s->alg == ALG_FIFO) victim_f = choose_victim_fifo(s);
                else if(s->alg == ALG_LRU) victim_f = choose_victim_lru(s);
                else /* OPTIMAL */ victim_f = choose_victim_optimal(s, future);

                int victim_page = s->frame_page[victim_f];
                // page out victim
                s->page_to_frame[victim_page] = -1;
                // page in new
                s->frame_page[victim_f] = page;
                s->page_to_frame[page] = victim_f;
                if(s->alg == ALG_LRU){ s->lru_age[victim_f] = s->time; }
                s->replacements++;
                print_fault_replaced(addr, page, victim_page, victim_f);
            }
        }

        // Advance OPT future pointer for this page (we just consumed index i)
        if(s->alg == ALG_OPTIMAL){ if(future[page].ptr < future[page].pos.size) future[page].ptr++; }
    }
}

static void usage(const char *prog){
    fprintf(stderr,
        "Usage: %s -a <fifo|lru|optimal> -n <frames> -f <trace file>\n",
        prog);
}

int main(int argc, char **argv){
    const char *afile=NULL; const char *tracefile=NULL; int nframes=-1; alg_t alg=ALG_FIFO;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i], "-a")==0 && i+1<argc){ afile = argv[++i]; }
        else if(strcmp(argv[i], "-n")==0 && i+1<argc){ nframes = atoi(argv[++i]); }
        else if(strcmp(argv[i], "-f")==0 && i+1<argc){ tracefile = argv[++i]; }
        else { usage(argv[0]); return 1; }
    }
    if(!afile || !tracefile || nframes<=0){ usage(argv[0]); return 1; }
    if(strcmp(afile, "fifo")==0) alg = ALG_FIFO;
    else if(strcmp(afile, "lru")==0) alg = ALG_LRU;
    else if(strcmp(afile, "optimal")==0) alg = ALG_OPTIMAL;
    else { fprintf(stderr, "Unknown algorithm: %s\n", afile); return 1; }

    // Read trace entirely
    FILE *fp = fopen(tracefile, "r");
    if(!fp){ perror("fopen trace"); return 1; }

    ivec_t trace_addrs; ivec_t trace_pages; ivec_init(&trace_addrs); ivec_init(&trace_pages);
    uint16_t addr;
    while(read_hex_address(fp, &addr)){
        ivec_push(&trace_addrs, (int)addr);
        int page = (addr >> 8) & 0xFF; // 256-byte pages
        ivec_push(&trace_pages, page);
    }
    fclose(fp);

    if(trace_pages.size==0){ fprintf(stderr, "Empty or invalid trace file.\n"); ivec_free(&trace_addrs); ivec_free(&trace_pages); return 1; }

    // Prepare OPT future lists
    future_list_t future[VIRTUAL_PAGES];
    for(int p=0;p<VIRTUAL_PAGES;++p){ ivec_init(&future[p].pos); future[p].ptr = 0; }
    if(alg == ALG_OPTIMAL){
        for(int i=0;i<trace_pages.size;++i){ int p = trace_pages.data[i]; ivec_push(&future[p].pos, i); }
    }

    // Init sim
    sim_t sim; sim_init(&sim, alg, nframes);

    // Run
    simulate(&sim, &trace_pages, &trace_addrs, future);

    // Summary
    printf("\n=== Summary ===\n");
    printf("Algorithm       : %s\n", alg_name(alg));
    printf("Frames          : %d (total physical = %d bytes)\n", sim.frames, sim.frames*PAGE_SIZE);
    printf("Total accesses  : %ld\n", sim.total_accesses);
    printf("Page hits       : %ld\n", sim.hits);
    printf("Page faults     : %ld\n", sim.faults);
    printf("Replacements    : %ld\n", sim.replacements);

    // Cleanup
    sim_free(&sim);
    for(int p=0;p<VIRTUAL_PAGES;++p) ivec_free(&future[p].pos);
    ivec_free(&trace_addrs); ivec_free(&trace_pages);
    return 0;
}
