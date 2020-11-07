/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#ifndef SCANNER_SET_H
#define SCANNER_SET_H

#include <map>
#include <set>
#include <string>
#include <vector>
#include <sstream>

#include "scanner_params.h"
#include "scanner_config.h"
#include "sbuf.h"

/**
 * \file
 * bulk_extractor scanner architecture.
 *
 * The scanner_set class implements loadable scanners from files and
 * keeps track of which are enabled and which are not.
 *
 * Sequence of operations:
 * 1. scanner_config is loaded with any name=value configurations.
 * 2. scanner_set() is created with the config. The scanner_set:
 *     - Loads any scanners from specified directories.
       - Processes all enable/disable commands to determine which scanners are enabled and disabled.
 * 3. Scanners are queried to determine which feature files they write to, and which histograms they created.
 * 4. Data is processed.
 * 5. Scanners are shutdown.
 * 6. Histograms are written out.
 *
 * Scanners are called with two parameters:
 * A reference to a scanner_params (SP) object.
 * A reference to a recursion_control_block (RCB) object.
 *
 * On startup, each scanner is called with a special SP and RCB.
 * The scanners respond by setting fields in the SP and returning.
 *
 * When executing, once again each scanner is called with the SP and RCB.
 * This is the only file that needs to be included for a scanner.
 *
 * \li \c phase_startup - scanners are loaded and register the names of the feature files they want.
 * \li \c phase_scan - each scanner is called to analyze 1 or more sbufs.
 * \li \c phase_shutdown - scanners are given a chance to shutdown
 *
 * The scanner_set references the feature_recorder_set, which is a set of feature_recorder objects.
 *
 * The scanner_set controls running of the scanners. It can run in a single-threaded mode, having a single
 * sbuf processed recursively within a single thread.
 * TODO: or it can be called with a threadpool.
 */

#include "packet_info.h"
#include "feature_recorder_set.h"

/**
 *  \class scanner_set
 *
 * scanner_set is a set of scanners that are loaded into memory. It consists of:
 *  - a set of commands for the scanners (we have the commands before the scanners are loaded)
 *  - a vector of the scanners
 *    - methods for adding scanners to the vector
 *  - the feature recorder set used by the scanners
 */

class scanner_set {
    // A boring class: can't copy or assign it.
    scanner_set(const scanner_set &s)=delete;
    scanner_set &operator=(const scanner_set &s)=delete;

    /**
     *  Commands whether to enable or disable a scanner. Typically created from parsing command-line arguments
     */
    struct scanner_command {
        enum command_t {DISABLE_ALL=0,ENABLE_ALL,DISABLE,ENABLE};
        scanner_command(const scanner_command &sc):command(sc.command),name(sc.name){};
        scanner_command(scanner_command::command_t c,const std::string &n):command(c),name(n){};
        command_t command {};
        std::string name  {};
    };

    // The commands for those scanners (enable, disable, options, etc.
    typedef std::vector<struct scanner_command> scanner_commands_t;
    scanner_commands_t scanner_commands {};
    void    process_scanner_commands(const std::vector<scanner_command> &scanner_commands); // process the commands

    /* These vectors keep track of the scanners that have been registered, enabled, and information about each scanner. */
    //typedef std::vector<scanner_t *> scanner_vector_t;
    //scanner_vector_t all_scanners;        // all of the scanners in the set
    //scanner_vector_t enabled_scanners;    // the scanners that are enabled

    typedef std::set<scanner_t *> scanner_set_set_t;
    scanner_set_set_t enabled_scanners {};    // the scanners that are enabled

    // a pointer to every scanner info in all of the scanners.
    // This provides all_scanners
    std::map<scanner_t *, const struct scanner_params::scanner_info *>scanner_info_db {};

    // The scanner_set's configuration for all the scanners that are loaded.
    const  scanner_config sc;

    /* The feature recorder set where the scanners outputs are stored */
    class    feature_recorder_set fs;

    /* Run-time configuration for all of the scanners (per-scanner configuration is stored in sc)
     * Default values are hard-coded below.
     */

    uint32_t max_depth             {7};       // maximum depth for recursive scans
    //uint32_t max_depth_seen        {0};       // maximum depth for recursive scans
    //mutable std::mutex max_depth_seenM     {};
    std::atomic<uint32_t> max_depth_seen {0};

    std::atomic<uint64_t> sbuf_seen {0}; // number of seen sbufs.

    uint32_t max_ngram             {10};      // maximum ngram size to scan for
    bool     dup_data_alerts       {false};  // notify when duplicate data is not processed
    uint64_t dup_data_encountered  {0}; // amount of dup data encountered
    std::ostream *sxml            {nullptr}; // if provided, a place to put XML tags when scanning
    //void     message_enabled_scanners(scanner_params::phase_t phase);
    scanner_params::phase_t     current_phase {scanner_params::PHASE_INIT};

    /* Implementation of transition from the init phase to the first scanning phase */
    void     add_enabled_scanner_histograms(); // called when switching from PHASE_INIT to PHASE_SCAN

public:;
    /* constructor and destructor */
    scanner_set(const scanner_config &, std::ostream *sxml=0);
    virtual ~scanner_set(){};

    /* PHASE_INIT */
    // Add scanners to the scanner set.

    //void set_debug(int debug);

    /* Scanners can be compiled in (which are passed to the constructor), loaded one-by-one from meory,
     * or loaded from a file, a directory, or a set of directories.
     * Loaded scanners are added to the 'scanners' vector.
     *
     * After the scanners are loaded, the scan starts.
     * Each scanner is called with scanner_params and a scanner control block as arguments.
     * See "scanner_params.h".
     */
    void    register_info(const scanner_params::scanner_info *si);
    void    add_scanner(scanner_t scanner);      // load a specific scanner in memory
    void    add_scanners(scanner_t * const *scanners_builtin); // load a nullptr array of scanners.
    void    add_scanner_file(std::string fn);    // load a scanner from a shared library file
    void    add_scanner_directory(const std::string &dirname); // load all scanners in the directory

    void    load_scanner_packet_handlers(); // after all scanners are loaded, this sets up the packet handlers.

    /* Control which scanners are enabled */
    void    set_scanner_enabled(const std::string &name, bool shouldEnable); // enable/disable a specific scanner
    void    set_scanner_enabled_all(bool shouldEnable); // enable/disable all scanners

    bool    is_scanner_enabled(const std::string &name); // report if it is enabled or not
    void    get_enabled_scanners(std::vector<std::string> &svector); // put names of the enabled scanners into the vector
    bool    is_find_scanner_enabled(); // return true if a find scanner is enabled

    /* These functions must be virtual so they can be called by dynamically loaded plugins */
    virtual scanner_t *get_scanner_by_name(const std::string &name) const;
    virtual feature_recorder *get_feature_recorder_by_name(const std::string &name) const;

    // report on the loaded scanners
    void     info_scanners(std::ostream &out,
                           bool detailed_info,bool detailed_settings,
                           const char enable_opt,const char disable_opt);

    /* Control the histograms set up during initialization phase */
    const std::string & get_input_fname() const;

    /* PHASE SCAN */
    void start_scan();
    void set_max_depth_seen(uint32_t max_depth_seen_);
    uint32_t get_max_depth_seen() const;

    /* Managing scanners */
    // TK - should this be an sbuf function?
    size_t find_ngram_size(const sbuf_t &sbuf) const;

    //void  get_scanner_feature_file_names(feature_file_names_t &feature_file_names);

    // enabling and disabling of scanners
    //void scanners_disable_all();                    // saves a command to disable all
    //void scanners_enable_all();                    // enable all of them
    static std::string ALL_SCANNERS;
    //void scanner_enable(const std::string &name); // saves a command to enable this scanner
    //void scanner_disable(const std::string &name); // saves a command to disable this scanner

    // returns the named scanner, or 0 if no scanner of that name


    // Scanners automatically get initted when they are loaded, so there is no scanners init or info phase
    // They are immediately ready to process sbufs and packets!
    // These trigger a move the PHASE_SCAN
    void     process_sbuf(const sbuf_t &sbuf);                              /* process for feature extraction */
    void     process_packet(const be13::packet_info &pi);
    scanner_params::phase_t get_current_phase() const { return current_phase;};
    // make the histograms
    // sxml is where to put XML from scanners that shutdown
    // the sxml should go to the constructor

    /* PHASE_SHUTDOWN */

    size_t   count_histograms() const;
    void     shutdown();
};

#endif
