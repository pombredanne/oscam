#include "module-stat.h"
#include "module-cccam.h"

#define UNDEF_AVG_TIME 80000
#define MAX_ECM_SEND_CACHE 16

static int stat_load_save;
static struct timeb nulltime;
static time_t last_housekeeping = 0;

void init_stat()
{
	cs_ftime(&nulltime);
	stat_load_save = -100;

	//checking config
	if (cfg->lb_nbest_readers < 2)
		cfg->lb_nbest_readers = DEFAULT_NBEST;
	if (cfg->lb_nfb_readers < 2)
		cfg->lb_nfb_readers = DEFAULT_NFB;
	if (cfg->lb_min_ecmcount < 1)
		cfg->lb_min_ecmcount = DEFAULT_MIN_ECM_COUNT;
	if (cfg->lb_max_ecmcount < 2)
		cfg->lb_max_ecmcount = DEFAULT_MAX_ECM_COUNT;
	if (cfg->lb_reopen_seconds < 10)
		cfg->lb_reopen_seconds = DEFAULT_REOPEN_SECONDS;
	if (cfg->lb_retrylimit <= 0)
		cfg->lb_retrylimit = DEFAULT_RETRYLIMIT;
	if (cfg->lb_stat_cleanup <= 0)
		cfg->lb_stat_cleanup = DEFAULT_LB_STAT_CLEANUP;
}

void load_stat_from_file()
{
	stat_load_save = 0;
	char buf[256];
	char *fname;
	FILE *file;
	if (!cfg->lb_savepath || !cfg->lb_savepath[0]) {
		sprintf(buf, "%s/stat", get_tmp_dir());
		fname = buf;
	}
	else
		fname = cfg->lb_savepath;
		
	file = fopen(fname, "r");
		
	if (!file) {
		cs_log("loadbalancer: can't read from file %s", fname);
		return;
	}
	cs_debug_mask(D_TRACE, "loadbalancer: load statistics from %s", fname);
	
	struct s_reader *rdr = NULL;
	READER_STAT *stat, *dup;
		
	int i=1;
	int count=0;
	do
	{
		stat = malloc(sizeof(READER_STAT));
		memset(stat, 0, sizeof(READER_STAT));
		i = fscanf(file, "%s rc %d caid %04hX prid %06lX srvid %04hX time avg %dms ecms %d last %ld\n",
			buf, &stat->rc, &stat->caid, &stat->prid, &stat->srvid, 
			&stat->time_avg, &stat->ecm_count, &stat->last_received);
			
		
		if (i > 5) {
			if (rdr == NULL || strcmp(buf, rdr->label) != 0) {
				for (rdr=first_reader; rdr ; rdr=rdr->next) {
					if (strcmp(rdr->label, buf) == 0) {
						break;
					}
				}
			}
			
			if (rdr != NULL && strcmp(buf, rdr->label) == 0) {
				if (!rdr->lb_stat)
					rdr->lb_stat = ll_create();
					
				//Duplicate check:
				dup = get_stat(rdr, stat->caid, stat->prid, stat->srvid);
				if (dup)
					free(stat); //already loaded
				else {
				
					ll_append(rdr->lb_stat, stat);
					count++;
				}
			}
			else 
			{
				cs_log("loadbalancer: statistics could not be loaded for %s", buf);
				free(stat);
			}
		}
		else if (i!=EOF && i>0)
		{
			cs_debug_mask(D_TRACE, "loadbalancer: statistics ERROR  %s rc=%d i=%d", buf, stat->rc, i);
			free(stat);
		}
		else
			free(stat);
	} while(i!=EOF && i>0);
	fclose(file);
	cs_debug_mask(D_TRACE, "loadbalancer: statistic loaded %d records", count);
}
/**
 * get statistic values for reader ridx and caid/prid/srvid
 */
READER_STAT *get_stat(struct s_reader *rdr, ushort caid, ulong prid, ushort srvid)
{
	if (!rdr->lb_stat)
		rdr->lb_stat = ll_create();

	LL_ITER *it = ll_iter_create(rdr->lb_stat);
	READER_STAT *stat = NULL;
	int i = 0;
	while ((stat = ll_iter_next(it))) {
		if (stat->caid==caid && stat->prid==prid && stat->srvid==srvid) {
			if (i > 5) { //Move to first if not under top 5:
				ll_iter_remove(it);
				ll_insert_at_nolock(rdr->lb_stat, stat, 0); //move to first!
			}
			break;
		}
		i++;
	}
	ll_iter_release(it);
	return stat;
}

/**
 * removes caid/prid/srvid from stat-list of reader ridx
 */
int remove_stat(struct s_reader *rdr, ushort caid, ulong prid, ushort srvid)
{
	if (!rdr->lb_stat)
		return 0;

	int c = 0;
	LL_ITER *it = ll_iter_create(rdr->lb_stat);
	READER_STAT *stat;
	while ((stat = ll_iter_next(it))) {
		if (stat->caid==caid && stat->prid==prid && stat->srvid==srvid) {
			ll_iter_remove_data(it);
			c++;
		}
	}
	ll_iter_release(it);
	return c;
}

/**
 * Calculates average time
 */
void calc_stat(READER_STAT *stat)
{
	int i;
	int c=0;
	long t = 0;
	for (i = 0; i < LB_MAX_STAT_TIME; i++) {
		if (stat->time_stat[i] > 0) {
			t += (long)stat->time_stat[i];
			c++;
		}
	}
	if (!c)
		stat->time_avg = UNDEF_AVG_TIME;
	else
		stat->time_avg = t / c;
}

/**
 * Saves statistik to /tmp/.oscam/stat.n where n is reader-index
 */
void save_stat_to_file()
{
	stat_load_save = 0;
	char buf[256];
	char *fname;
	if (!cfg->lb_savepath || !cfg->lb_savepath[0]) {
		sprintf(buf, "%s/stat", get_tmp_dir());
		fname = buf;
	}
	else
		fname = cfg->lb_savepath;
		
	FILE *file = fopen(fname, "w");
	
	if (!file) {
		cs_log("can't write to file %s", fname);
		return;
	}

	int count=0;
	struct s_reader *rdr;
	for (rdr=first_reader; rdr ; rdr=rdr->next) {
		
		if (rdr->lb_stat) {
			LL_ITER *it = ll_iter_create(rdr->lb_stat);
			READER_STAT *stat;
			while ((stat = ll_iter_next(it))) {
				
				fprintf(file, "%s rc %d caid %04hX prid %06lX srvid %04hX time avg %dms ecms %d last %ld\n",
					rdr->label, stat->rc, stat->caid, stat->prid, 
					stat->srvid, stat->time_avg, stat->ecm_count, stat->last_received);
				count++;
			}
			ll_iter_release(it);
		}
	}
	
	fclose(file);
	cs_log("loadbalancer: statistic saved %d records to %s", count, fname);
}

/**
 * Adds caid/prid/srvid to stat-list for reader ridx with time/rc
 */
void add_stat(struct s_reader *rdr, ECM_REQUEST *er, int ecm_time, int rc)
{
	if (!rdr || !er)
		return;
	READER_STAT *stat = get_stat(rdr, er->caid, er->prid, er->srvid);
	if (!stat) {
		stat = malloc(sizeof(READER_STAT));
		memset(stat, 0, sizeof(READER_STAT));
		stat->caid = er->caid;
		stat->prid = er->prid;
		stat->srvid = er->srvid;
		stat->time_avg = UNDEF_AVG_TIME; //dummy placeholder
		ll_append(rdr->lb_stat, stat);
	}

	//inc ecm_count if found, drop to 0 if not found:
	// rc codes:
	// 0 = found       +
	// 1 = cache1      #
	// 2 = cache2      #
	// 3 = emu         +
	// 4 = not found   -
	// 5 = timeout     -2
	// 6 = sleeping    #
	// 7 = fake        #
	// 8 = invalid     #
	// 9 = corrupt     #
	// 10= no card     #
	// 11= expdate     #
	// 12= disabled    #
	// 13= stopped     #
	// 100= unhandled  #
	//        + = adds statistic values
	//        # = ignored because of duplicate values, temporary failures or softblocks
	//        - = causes loadbalancer to block this reader for this caid/prov/sid
	//        -2 = causes loadbalancer to block if happens too often
	
	if (stat->ecm_count < 0)
		stat->ecm_count=0;
		
	if (rc == 0) { //found
		stat->rc = 0;
		stat->ecm_count++;
		stat->last_received = time(NULL);
		stat->request_count = 0;
		
		//FASTEST READER:
		stat->time_idx++;
		if (stat->time_idx >= LB_MAX_STAT_TIME)
			stat->time_idx = 0;
		stat->time_stat[stat->time_idx] = ecm_time;
		calc_stat(stat);

		//OLDEST READER now set by get best reader!
		
		
		//USAGELEVEL:
		int ule = rdr->lb_usagelevel_ecmcount;
		if (ule > 0 && ((ule / cfg->lb_min_ecmcount) > 0)) //update every MIN_ECM_COUNT usagelevel:
		{
			time_t t = (time(NULL)-rdr->lb_usagelevel_time);
			rdr->lb_usagelevel = 1000/(t<1?1:t);
			ule = 0;
		}
		if (ule == 0)
			rdr->lb_usagelevel_time = time(NULL);
		rdr->lb_usagelevel_ecmcount = ule+1;
		
	}
	else if (rc == 1 || rc == 2) { //cache
		//no increase of statistics here, cachetime is not real time
		stat->last_received = time(NULL);
		stat->request_count = 0;
	}
	else if (rc == 4) { //not found
		stat->rc = rc;
		//stat->last_received = time(NULL); do not change time, this would prevent reopen
		//stat->ecm_count = 0; Keep ecm_count!
	}
	else if (rc == 5) { //timeout
		stat->request_count++;
		stat->last_received = time(NULL);

		//add timeout to stat:
		if (ecm_time<=0)
			ecm_time = cfg->ctimeout;
		stat->time_idx++;
		if (stat->time_idx >= LB_MAX_STAT_TIME)
			stat->time_idx = 0;
		stat->time_stat[stat->time_idx] = ecm_time;
		calc_stat(stat);
	}
	else
	{
		if (rc >= 0)
			cs_debug_mask(D_TRACE, "loadbalancer: not handled stat for reader %s: rc %d caid %04hX prid %06lX srvid %04hX time %dms usagelevel %d",
				rdr->label, rc, er->caid, er->prid, er->srvid, ecm_time, rdr->lb_usagelevel);
	
		return;
	}
	
	housekeeping_stat(0);
		
	cs_debug_mask(D_TRACE, "loadbalancer: adding stat for reader %s: rc %d caid %04hX prid %06lX srvid %04hX time %dms usagelevel %d",
				rdr->label, rc, er->caid, er->prid, er->srvid, ecm_time, rdr->lb_usagelevel);
	
	if (cfg->lb_save) {
		stat_load_save++;
		if (stat_load_save > cfg->lb_save) {
			save_stat_to_file();
		}
	}
}

void reset_stat(ushort caid, ulong prid, ushort srvid)
{
	//cs_debug_mask(D_TRACE, "loadbalance: resetting ecm count");
	struct s_reader *rdr;
	for (rdr=first_reader; rdr ; rdr=rdr->next) {
		if (rdr->lb_stat && rdr->client) {
			READER_STAT *stat = get_stat(rdr, caid, prid, srvid);
			if (stat) {
				if (stat->ecm_count > 0)
					stat->ecm_count = 1; //not zero, so we know it's decodeable
				stat->rc = 0;
				stat->request_count = 0;
			}
		}
	}
}

int has_ident(FTAB *ftab, ECM_REQUEST *er) {

	if (!ftab || !ftab->filts)
		return 0;
		
        int j, k;

        for (j = 0; j < ftab->nfilts; j++) {
		if (ftab->filts[j].caid) {
			if (ftab->filts[j].caid==er->caid) { //caid matches!

				int nprids = ftab->filts[j].nprids;
                                if (!nprids) // No Provider ->Ok
                                        return 1;

				for (k = 0; k < nprids; k++) {
					ulong prid = ftab->filts[j].prids[k];
					if (prid == er->prid) { //Provider matches
						return 1;
                                        }
                                }
                        }
                }
        }
        return 0; //No match!
}



/**	
 * Gets best reader for caid/prid/srvid.
 * Best reader is evaluated by lowest avg time but only if ecm_count > cfg->lb_min_ecmcount (5)
 * Also the reader is asked if he is "available"
 * returns ridx when found or -1 when not found
 */
int get_best_reader(ECM_REQUEST *er)
{
	int i;
	int rdr_count = er->rdr_count;
	uint8 result[rdr_count];
	memset(result, 0, sizeof(result));
	int re[rdr_count];
	int ti[rdr_count];
	
	//resulting values:
	memset(re, 0, sizeof(re));
	memset(ti, 0, sizeof(ti));
	
	struct timeb new_nulltime;
	memset(&new_nulltime, 0, sizeof(new_nulltime));
	time_t current_time = time(NULL);
	
	int current = -1;
	
	READER_STAT *stat = NULL;
	struct s_reader *rdr;
	
	int nlocal_readers = 0;

#ifdef WITH_DEBUG 
	char rdrs[rdr_count+1];
	for (i=0;i<rdr_count;i++)
		rdrs[i] = er->matching_rdr[i]+'0';
	rdrs[rdr_count]=0;
	cs_debug_mask(D_TRACE, "loadbalancer: client %s for %04X/%06X/%04X: valid readers: %s", 
		username(er->client), er->caid, er->prid, er->srvid, rdrs);
#endif	
	
	for (i=0,rdr=first_reader; rdr ; rdr=rdr->next, i++) {
		if (i >= rdr_count)
			break;
			
		if (er->matching_rdr[i]) {
 			int weight = rdr->lb_weight <= 0?100:rdr->lb_weight;
				
			stat = get_stat(rdr, er->caid, er->prid, er->srvid);
			if (!stat) {
				cs_debug_mask(D_TRACE, "loadbalancer: starting statistics for reader %s", rdr->label);
				add_stat(rdr, er, 1, -1);
				result[i] = 1; //no statistics, this reader is active (now) but we need statistics first!
				continue;
			}
			
			ti[i] = stat->time_avg;
			
			if (stat->ecm_count < 0||(stat->ecm_count > cfg->lb_max_ecmcount && stat->time_avg > (int)cfg->ftimeout)) {
				cs_debug_mask(D_TRACE, "loadbalancer: max ecms (%d) reached by reader %s, resetting statistics", cfg->lb_max_ecmcount, rdr->label);
				reset_stat(er->caid, er->prid, er->srvid);
				result[i] = 1;//max ecm reached, get new statistics
			}
				
			if (stat->rc == 0 && stat->ecm_count < cfg->lb_min_ecmcount) {
				cs_debug_mask(D_TRACE, "loadbalancer: reader %s needs more statistics", rdr->label);
				result[i] = 1; //need more statistics!
			}
			
			int hassrvid = has_srvid(rdr->client, er) || has_ident(&rdr->ftab, er);
			
			if (!hassrvid && stat->rc == 0 && stat->request_count > cfg->lb_min_ecmcount) { // 5 unanswered requests or timeouts?
				cs_debug_mask(D_TRACE, "loadbalancer: reader %s does not answer, blocking", rdr->label);
				add_stat(rdr, er, 1, 4); //reader marked as unuseable 
				result[i] = 0;
				continue;
			}
				

			//Reader can decode this service (rc==0) and has lb_min_ecmcount ecms:
			if (stat->rc == 0 || hassrvid) {
				if (cfg->preferlocalcards && !(rdr->typ & R_IS_NETWORK))
					nlocal_readers++; //Prefer local readers!
			
				switch (cfg->lb_mode) {
				default:
				case LB_NONE:
					//cs_debug_mask(D_TRACE, "loadbalance disabled");
					result[i] = 1;
					current = 1;
					break;
				case LB_FASTEST_READER_FIRST:
					current = stat->time_avg * 100 / weight;
					break;
				case LB_OLDEST_READER_FIRST:
					if (!rdr->lb_last.time)
						rdr->lb_last = nulltime;
					current = (1000*(rdr->lb_last.time-nulltime.time)+
						rdr->lb_last.millitm-nulltime.millitm);
					if (!new_nulltime.time || (1000*(rdr->lb_last.time-new_nulltime.time)+
						rdr->lb_last.millitm-new_nulltime.millitm) < 0)
						new_nulltime = rdr->lb_last;
					break;
				case LB_LOWEST_USAGELEVEL:
					current = rdr->lb_usagelevel * 100 / weight;
					break;
				}
#ifdef WEBIF
				rdr->lbvalue = current;
#endif
				if (rdr->ph.c_available
						&& !rdr->ph.c_available(rdr,
								AVAIL_CHECK_LOADBALANCE)) {
					current=current*2;
				}
				if (current < 1)
					re[i]=1;
				else
					re[i] = current;
			}
		}
	}

	int nbest_readers = cfg->lb_nbest_readers;
	int nfb_readers = cfg->lb_nfb_readers;
	if (nlocal_readers > nbest_readers) { //if we have local readers, we prefer them!
		nlocal_readers = nbest_readers;
		nbest_readers = 0;	
	}
	else
		nbest_readers = nbest_readers-nlocal_readers;
	
	int best_ridx = -1;
	struct s_reader *best_rdr = NULL;
	struct s_reader *best_rdri = NULL;

	int n=0;
	while (1) {
		int best_idx=-1;
		int best=0;
		int j;
		struct s_reader *rdr2;
		for (j=0,rdr2=first_reader; rdr2 ; rdr2=rdr2->next, j++) {
			if (j>=rdr_count)
				break;
			if (nlocal_readers && (rdr2->typ & R_IS_NETWORK))
				continue;
						
			if (re[j] && (!best || re[j] < best)) {
				best_idx=j;
				best=re[j];
				best_rdri = rdr2;
			}
		}
		if (best_idx<0)
			break;
		else {
			n++;
			re[best_idx]=0;
			if (nlocal_readers) {//primary readers, local
				nlocal_readers--;
				result[best_idx] = 1;
				//OLDEST_READER:
				cs_ftime(&best_rdri->lb_last);
				if (best_ridx <0) {
					best_ridx = best_idx;
					best_rdr = best_rdri;
				}
			}
			else if (nbest_readers) {//primary readers, other
				nbest_readers--;
				result[best_idx] = 1;
				//OLDEST_READER:
				cs_ftime(&best_rdri->lb_last);
				if (best_ridx <0) {
					best_ridx = best_idx;
					best_rdr = best_rdri;
				}
			}
			else if (nfb_readers) { //fallbacks:
				nfb_readers--;
				if (!result[best_idx])
					result[best_idx] = 2;
			}
			else
				break;
		}
	}
	
	if (!n) //no best reader found? reopen if we have ecm_count>0
	{
		cs_debug_mask(D_TRACE, "loadbalancer: NO MATCHING READER FOUND, reopen last valid:");
	        for (i=0,rdr=first_reader; rdr ; rdr=rdr->next, i++) { 
	        	if (i>=rdr_count)
	        		 break;
        		if (er->matching_rdr[i]) { //primary readers 
	        		stat = get_stat(rdr, er->caid, er->prid, er->srvid); 
	        		if (stat && stat->ecm_count>0) {
	        			result[i] = 1;
	        			n++;
	        			cs_debug_mask(D_TRACE, "loadbalancer: reopened reader %s", rdr->label);
				}
			}
		}
		cs_debug_mask(D_TRACE, "loadbalancer: reopened %d readers", n);
	}

	memcpy(er->matching_rdr, result, sizeof(result));
#ifdef WITH_DEBUG 

	for (i=0;i<rdr_count;i++)
		rdrs[i] = er->matching_rdr[i]+'0';
	rdrs[rdr_count]=0;
	cs_debug_mask(D_TRACE, "loadbalancer: client %s for %04X/%06X/%04X: %s readers: %s", 
		username(er->client), er->caid, er->prid, er->srvid,
		best_rdr?best_rdr->label:"NONE", rdrs);
#endif	

	//reopen other reader only if responsetime>retrylimit:
	int reopen = (best_ridx>=0 && ti[best_ridx] && (ti[best_ridx] > cfg->lb_retrylimit));
	if (reopen)
		cs_debug_mask(D_TRACE, "loadbalancer: reader %s reached retrylimit (%dms), reopening other readers", best_rdr->label, ti[best_ridx]);
	
        for (i=0,rdr=first_reader; rdr ; rdr=rdr->next, i++) { 
        	if (i>=rdr_count)
        		break;
        		
		if (!er->matching_rdr[i]) 
			continue;
			
        	if (result[i] == 1) { //primary readers 
        		stat = get_stat(rdr, er->caid, er->prid, er->srvid); 
       			//algo for finding unanswered requests (newcamd reader for example:) 
        		if (stat && current_time > stat->last_received+(time_t)(cfg->ctimeout/1000)) { 
        			stat->request_count++; 
        			stat->last_received = current_time;
        			cs_debug_mask(D_TRACE, "loadbalancer: reader %s increment request count to %d", rdr->label, stat->request_count);
			} 
		} else if (reopen) { //retrylimit reached:
			stat = get_stat(rdr, er->caid, er->prid, er->srvid); 
			if (stat && stat->rc != 0) {
				int seconds = cfg->lb_reopen_seconds;
				if (!rdr->audisabled && (er->client->autoau || er->client->aureader == rdr))
					seconds = seconds/10; //reopen faster if reader is a au reader
				
				if (stat->last_received+seconds < current_time) { //Retrying reader every (900/conf) seconds
					stat->last_received = current_time;
					result[i] = 1;
					cs_log("loadbalancer: retrying reader %s", rdr->label);
				}
			}
		}
        }
        
	if (new_nulltime.time)
		nulltime = new_nulltime;
		
	return 1;
}

/**
 * clears statistic of reader ridx.
 **/
void clear_reader_stat(struct s_reader *rdr)
{
	if (!rdr->lb_stat) 
		return;

	ll_clear_data(rdr->lb_stat);
}

void clear_all_stat()
{
	struct s_reader *rdr;
	for (rdr=first_reader; rdr ; rdr=rdr->next) { 
		clear_reader_stat(rdr);
	}
}

void housekeeping_stat(int force)
{
	time_t now = time(NULL);
	if (!force && now/60/60 == last_housekeeping/60/60) //only clean once in an hour
		return;
	
	last_housekeeping = now;
	
	time_t cleanup_time = now - (cfg->lb_stat_cleanup*60*60);
	
	struct s_reader *rdr;
	for (rdr=first_reader; rdr ; rdr=rdr->next) { 
		if (rdr->lb_stat) {
			LL_ITER *itr = ll_iter_create(rdr->lb_stat);
			READER_STAT *stat;
			while ((stat=ll_iter_next(itr))) {
				
				if (stat->last_received < cleanup_time)
					ll_iter_remove_data(itr);
			}
			
			ll_iter_release(itr);
		}
	}
}
