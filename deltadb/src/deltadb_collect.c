/*
Copyright (C) 2012- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include "deltadb_stream.h"
#include "deltadb_expr.h"

#include "jx_database.h"
#include "jx_print.h"
#include "jx_parse.h"

#include "hash_table.h"
#include "debug.h"
#include "getopt.h"
#include "cctools.h"
#include "list.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdarg.h>
#include <ctype.h>

struct deltadb {
	struct hash_table *table;
	const char *logdir;
	FILE *logfile;
	int epoch_time_mode;
	struct deltadb_expr *filter_exprs;
	struct deltadb_expr *where_exprs;
	struct list * output_exprs;
};

struct deltadb * deltadb_create( const char *logdir )
{
	struct deltadb *db = malloc(sizeof(*db));
	memset(db,0,sizeof(*db));
	db->table = hash_table_create(0,0);
	db->logdir = logdir;
	db->logfile = 0;
	db->output_exprs = list_create();
	return db;
}

/* Get a complete checkpoint file and reconstitute the state of the table. */

static int checkpoint_read( struct deltadb *db, const char *filename )
{
	FILE * file = fopen(filename,"r");
	if(!file) return 0;

	/* Load the entire checkpoint into one json object */
	struct jx *jcheckpoint = jx_parse_stream(file);

	fclose(file);

	if(!jcheckpoint || jcheckpoint->type!=JX_OBJECT) {
		fprintf(stderr,"checkpoint %s is not a valid json document!\n",filename);
		jx_delete(jcheckpoint);
		return 0;
	}

	/* For each key and value, move the value over to the hash table. */

	/* Skip objects that don't match the filter. */

	struct jx_pair *p;
	for(p=jcheckpoint->pairs;p;p=p->next) {
		jx_assert(p->key,JX_STRING);
		if(!deltadb_expr_matches(db->filter_exprs,p->value)) continue;
		hash_table_insert(db->table,p->key->string_value,p->value);
		p->value = 0;
	}

	/* Delete the leftover object with empty pairs. */

	jx_delete(jcheckpoint);

	return 1;
}

int deltadb_create_event( struct deltadb *db, const char *key, struct jx *jobject )
{
	if(!deltadb_expr_matches(db->filter_exprs,jobject)) return 1;
	hash_table_insert(db->table,key,jobject);
	return 1;
}

int deltadb_delete_event( struct deltadb *db, const char *key )
{
	jx_delete(hash_table_remove(db->table,key));
	return 1;
}

int deltadb_update_event( struct deltadb *db, const char *key, const char *name, struct jx *jvalue )
{
	struct jx * jobject = hash_table_lookup(db->table,key);
	if(!jobject) return 1;

	struct jx *jname = jx_string(name);
	jx_delete(jx_remove(jobject,jname));
	jx_insert(jobject,jname,jvalue);
	return 1;
}

int deltadb_remove_event( struct deltadb *db, const char *key, const char *name )
{
	struct jx *jobject = hash_table_lookup(db->table,key);
	if(!jobject) return 1;

	struct jx *jname = jx_string(name);
	jx_delete(jx_remove(jobject,jname));
	jx_delete(jname);
	return 1;
}

int deltadb_time_event( struct deltadb *db, time_t starttime, time_t stoptime, time_t current )
{
	if(current>stoptime) return 0;

	/* If no output has been defined, skip this. */

	if(!list_size(db->output_exprs)) return 1;

	/* For each item in the table... */

	char *key;
	struct jx *jobject;
	hash_table_firstkey(db->table);
	while(hash_table_nextkey(db->table,&key,(void**)&jobject)) {

		/* Skip if the where expression doesn't match */

		if(!deltadb_expr_matches(db->where_exprs,jobject)) continue;

		/* Emit the current time */

		if(db->epoch_time_mode) {
			printf("%lld\t",(long long) current);
		} else {
			char str[32];
			strftime(str,sizeof(str),"%F %T",localtime(&current));
			printf("%s\t",str);
		}

		/* For each output expression, compute the value and print. */

		struct list_node *n;
		char *str;
		for(n=db->output_exprs->head;n;n=n->next) {

			struct jx *jvalue = jx_lookup(jobject,n->data);
			if(jvalue) {
				str = jx_print_string(jvalue);
			} else {
				str = strdup("null");
			}
			printf("%s\t",str);
			free(str);
		}

		printf("\n");
	}

	return 1;
}

int deltadb_post_event( struct deltadb *db, const char *line )
{
	if(!list_size(db->output_exprs)) {
		printf("%s",line);
	}
	return 1;
}

/*
Play the log from starttime to stoptime by opening the appropriate
checkpoint file and working ahead in the various log files.
*/

static int log_play_time( struct deltadb *db, time_t starttime, time_t stoptime )
{
	char filename[1024];
	int file_errors = 0;

	struct tm *starttm = localtime(&starttime);

	int year = starttm->tm_year + 1900;
	int day = starttm->tm_yday;

	struct tm *stoptm = localtime(&stoptime);

	int stopyear = stoptm->tm_year + 1900;
	int stopday = stoptm->tm_yday;

	sprintf(filename,"%s/%d/%d.ckpt",db->logdir,year,day);
	checkpoint_read(db,filename);

	while(1) {
		sprintf(filename,"%s/%d/%d.log",db->logdir,year,day);
		FILE *file = fopen(filename,"r");
		if(!file) {
			file_errors += 1;
			fprintf(stderr,"couldn't open %s: %s\n",filename,strerror(errno));
			if (file_errors>5)
				break;
		} else {
			int keepgoing = deltadb_process_stream(db,file,starttime,stoptime);
			starttime = 0;

			fclose(file);

			// If we reached the endtime in the file, stop.
			if(!keepgoing) break;
		}

		day++;
		if(day>=365) {
			year++;
			day = 0;
		}

		// If we have passed the file, stop.
		if(year>=stopyear && day>stopday) break;
	}

	return 1;
}

int suffix_to_multiplier( char suffix )
{
	switch(tolower(suffix)) {
	case 'y': return 60*60*24*365;
	case 'w': return 60*60*24*7;
	case 'd': return 60*60*24;
	case 'h': return 60*60;
	case 'm': return 60;
	default: return 1;
	}
}

time_t parse_time( const char *str, time_t current )
{
	struct tm t;
	int count;
	char suffix[2];
	int n;

	memset(&t,0,sizeof(t));

	if(!strcmp(str,"now")) {
		return current;
	}

	n = sscanf(str, "%d%[yYdDhHmMsS]", &count, suffix);
	if(n==2) {
		return current - count*suffix_to_multiplier(suffix[0]);
	}

	n = sscanf(str, "%d-%d-%d@%d:%d:%d", &t.tm_year,&t.tm_mon,&t.tm_mday,&t.tm_hour,&t.tm_min,&t.tm_sec);
	if(n==6) {
		if (t.tm_hour>23)
			t.tm_hour = 0;
		if (t.tm_min>23)
			t.tm_min = 0;
		if (t.tm_sec>23)
			t.tm_sec = 0;

		t.tm_year -= 1900;
		t.tm_mon -= 1;

		return mktime(&t);
	}

	n = sscanf(str, "%d-%d-%d", &t.tm_year,&t.tm_mon,&t.tm_mday);
	if(n==3) {
		t.tm_year -= 1900;
		t.tm_mon -= 1;

		return mktime(&t);
	}

	return 0;
}

static struct option long_options[] =
{
	{"db", required_argument, 0, 'D'},
	{"output", required_argument, 0, 'o'},
	{"where", required_argument, 0,'w'},
	{"filter", required_argument, 0,'f'},
	{"from", required_argument, 0, 'F'},
	{"to", required_argument, 0, 'T'},
	{"at", required_argument, 0, 'A'},
	{"every", required_argument, 0, 'e'},
	{"epoch-time", no_argument, 0, 't'},
	{"version", no_argument, 0, 'v'},
	{"help", no_argument, 0, 'h'},
	{0,0,0,0}
};

void show_help()
{
	printf("use: deltadb_query [options]\n");
	printf("Where options are:\n");
	printf("  --db <path>         Path to the database directory. (required)\n");
	printf("  --output <expr>     Output this expression. (multiple)\n");
	printf("  --where <expr>      Only output records matching this expression.\n");
	printf("  --filter <expr>     Only process records matching this expression.\n");
	printf("  --from <time>       Begin query at this absolute time. (required)\n");
	printf("  --to <time>         End query at this absolute time.\n");
	printf("  --at <time>         Query once at this absolute time.\n");
	printf("  --every <interval>  Compute output at this time interval.\n");
	printf("  --epoch-time        Display time column in Unix epoch format.\n");
	printf("  --version           Show software version.\n");
	printf("  --help              Show this help text.\n");
}

int main( int argc, char *argv[] )
{
	const char *dbdir=0;
	struct deltadb_expr *where_exprs = 0;
	struct deltadb_expr *filter_exprs = 0;
	struct list *output_exprs = list_create();
	time_t start_time = 0;
	time_t stop_time = 0;
	time_t at_time =0;
	int every_interval= 0;
	int epoch_time_mode = 0;

	time_t current = time(0);

	int c;

	while((c=getopt_long(argc,argv,"D:o:w:f:F:T:A:e:tvh",long_options,0))!=-1) {
		switch(c) {
		case 'D':
			dbdir = optarg;
			break;
		case 'o':
			list_push_tail(output_exprs,strdup(optarg));
			break;
		case 'w':
			where_exprs = deltadb_expr_create(optarg,where_exprs);
			break;
		case 'f':
			filter_exprs = deltadb_expr_create(optarg,filter_exprs);
			break;
		case 'F':
			start_time = parse_time(optarg,current);
			break;
		case 'T':
			stop_time = parse_time(optarg,current);
			break;
		case 'A':
			at_time = parse_time(optarg,current);
			break;
		case 'e':
			every_interval = parse_time(optarg,current);
			break;
		case 't':
			epoch_time_mode = 1;
			break;
		case 'v':
			cctools_version_print(stdout,"deltadb_query");
			break;
		case 'h':
			show_help();
			break;
		}
	}

	if(!dbdir) {
		fprintf(stderr,"deltadb_query: --db argument is required\n");
		return 1;
	}

	if(start_time==0) {
		fprintf(stderr,"deltadb_query: invalid start time\n");
		return 1;
	}

	if(stop_time==0) {
		stop_time = time(0);
	}

	struct deltadb *db = deltadb_create(dbdir);

	db->where_exprs = where_exprs;
	db->filter_exprs = filter_exprs;
	db->epoch_time_mode = epoch_time_mode;
	db->output_exprs = output_exprs;

	log_play_time(db,start_time,stop_time);

	return 0;
}
