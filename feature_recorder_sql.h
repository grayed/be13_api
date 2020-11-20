#ifndef FEATURE_RECORDER_SQL_H
#define FEATURE_RECORDER_SQL_H

#include <cinttypes>
#include <cassert>

#include <string>
#include <regex>
#include <set>
#include <map>
#include <thread>
#include <iostream>
#include <fstream>
#include <atomic>

#include "feature_recorder.h"
#include "atomic_set_map.h"
#include "pos0.h"
#include "sbuf.h"
#include "histogram.h"

#include <sqlite3.h>

class feature_recorder_sql : public feature_recorder {
    static void truncate_at(std::string &line, char ch) {
        size_t pos = line.find(ch);
        if(pos != std::string::npos) line.resize(pos);
    };

    struct besql_stmt {
        besql_stmt(const besql_stmt &)=delete;
        besql_stmt &operator=(const besql_stmt &)=delete;
        std::mutex         Mstmt {};
        sqlite3_stmt *stmt {};      // the prepared statement
        besql_stmt(sqlite3 *db3,const char *sql);
        virtual ~besql_stmt();
        void insert_feature(const pos0_t &pos, // insert it into this table!
                            const std::string &feature,const std::string &feature8, const std::string &context);
    };
#if defined(HAVE_SQLITE3_H) and defined(HAVE_LIBSQLITE3)
    //virtual void dump_histogram_sqlite3(const histogram_def &def,void *user,feature_recorder::dump_callback_t cb) const;
#endif
public:
    feature_recorder_sql(class feature_recorder_set &fs, const std::string &name);
    virtual ~feature_recorder_sql();

};
#endif
