/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include "config.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <cstdarg>
#include <regex>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <filesystem>

#include "feature_recorder.h"
#include "feature_recorder_set.h"
#include "word_and_context_list.h"
#include "unicode_escape.h"
#include "utils.h"

//int64_t feature_recorder::offset_add   = 0;
//std::string  feature_recorder::banner_file;
//uint32_t feature_recorder::opt_max_context_size=1024*1024;
//uint32_t feature_recorder::opt_max_feature_size=1024*1024;
//uint32_t feature_recorder::debug = DEBUG_PEDANTIC; // default during development
//std::thread::id feature_recorder::main_thread_id = std::this_thread::get_id();
const std::string feature_recorder::MAX_DEPTH_REACHED_ERROR_FEATURE {"process_extract: MAX DEPTH REACHED"};
const std::string feature_recorder::MAX_DEPTH_REACHED_ERROR_CONTEXT {""};

const std::string feature_recorder::CARVE_MODE_DESCRIPTION {"0=carve none; 1=carve encoded; 2=carve all"};

const std::string feature_recorder::NO_CARVED_FILE {""};

static inline bool isodigit(char c)
{
    return c>='0' && c<='7';
}

/* Feature recorder functions that don't have anything to do with files  or SQL databases */
static inline int hexval(char ch)
{
    switch (ch) {
    case '0': return 0;
    case '1': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'a': case 'A': return 10;
    case 'b': case 'B': return 11;
    case 'c': case 'C': return 12;
    case 'd': case 'D': return 13;
    case 'e': case 'E': return 14;
    case 'f': case 'F': return 15;
    }
    return 0;
}

feature_recorder::feature_recorder(class feature_recorder_set &fs_, const struct feature_recorder_def def):
    fs(fs_),name(def.name),flags(def.flags)
{
}

feature_recorder::~feature_recorder()
{
}

/**
 * Unquote Python or octal-style quoting of a string
 */
std::string feature_recorder::unquote_string(const std::string &s)
{
    size_t len = s.size();
    if(len<4) return s;                 // too small for a quote

    std::string out;
    for (size_t i=0;i<len;i++){
        /* Look for octal coding */
        if(i+3<len && s[i]=='\\' && isodigit(s[i+1]) && isodigit(s[i+2]) && isodigit(s[i+3])){
            uint8_t code = (s[i+1]-'0') * 64 + (s[i+2]-'0') * 8 + (s[i+3]-'0');
            out.push_back(code);
            i += 3;                     // skip over the digits
            continue;
        }
        /* Look for hex coding */
        if(i+3<len && s[i]=='\\' && s[i+1]=='x' && isxdigit(s[i+2]) && isxdigit(s[i+3])){
            uint8_t code = (hexval(s[i+2])*16) | hexval(s[i+3]);
            out.push_back(code);
            i += 3;                     // skip over the digits
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

/**
 * Get the feature which is defined as being between a \t and [\t\n]
 */

/*static*/ std::string feature_recorder::extract_feature(const std::string &line)
{
    size_t tab1 = line.find('\t');
    if(tab1==std::string::npos) return "";   // no feature
    size_t feature_start = tab1+1;
    size_t tab2 = line.find('\t',feature_start);
    if(tab2!=std::string::npos) return line.substr(feature_start,tab2-feature_start);
    return line.substr(feature_start);  // no context to remove
}

const std::string &feature_recorder::get_outdir() const // cannot be inline becuase it accesses fs
{
    return fs.get_outdir();
}

/*
 * Returns a filename for this feature recorder with a specific suffix.
 */
const std::string feature_recorder::fname_in_outdir(std::string suffix, int count) const
{
    std::string base_name = fs.get_outdir() + "/" + this->name;
    if (suffix.size() > 0){
        base_name += "_"+suffix;
    }
    if (count == NO_COUNT) return base_name+".txt";
    if (count != NEXT_COUNT){
        return base_name+"_"+std::to_string(count)+".txt";
    }
    /* Probe for a file that we can create. When we create it, return the name.
     * Yes, this created a TOCTOU error. We should return the open file descriptor.
     */
    for(int i=0;i<1000000;i++){
        std::string fname = base_name + (i>0 ? "_"+std::to_string(i) : "") + ".txt";
        int fd = open(fname.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
        if ( fd >= 0 ){
            /* Created the file. close it. and return.*/
            close(fd);
            return fname;
        }
    }
    throw std::runtime_error("it is unlikely that there are a million files, so this is probably a logic error.");
}





 /**
 * the main entry point of writing a feature and its context to the feature file.
 * processes the stop list
 */

void feature_recorder::quote_if_necessary(std::string &feature,std::string &context) const
{
    /* By default quote string that is not UTF-8, and quote backslashes. */
    bool escape_bad_utf8  = true;
    bool escape_backslash = true;

    if (flags.no_quote) {          // don't quote either
        escape_bad_utf8  = false;
        escape_backslash = false;
    }

    if (flags.xml) {               // only quote bad utf8
        escape_bad_utf8  = true;
        escape_backslash = false;
    }

    feature = validateOrEscapeUTF8(feature,
                                   escape_bad_utf8, escape_backslash,
                                   validateOrEscapeUTF8_validate);

    if (feature.size() > fs.opt_max_feature_size) {
        feature.resize(fs.opt_max_feature_size);
    }

    if ( flags.no_context == false) {
        context = validateOrEscapeUTF8(context, escape_bad_utf8,
                                       escape_backslash, validateOrEscapeUTF8_validate);
        if (context.size() > fs.opt_max_context_size) {
            context.resize(fs.opt_max_context_size);
        }
    }
}

/*
 * write0:
 */
void feature_recorder::write0(const std::string &str)
{
}

/*
 * Write: keep track of count of features written.
 */
void feature_recorder::write0(const pos0_t &pos0, const std::string &feature, const std::string &context)
{
    features_written += 1;
}

/**
 * write() is the main entry point for writing a feature at a given position with context.
 * write() checks the stoplist and escapes non-UTF8 characters, then calls write0().
 */
void feature_recorder::write(const pos0_t &pos0, const std::string &feature_, const std::string &context_)
{
    if (fs.flags.disabled) return;           // disabled

    if (fs.flags.pedantic){
        if (feature_.size() > fs.opt_max_feature_size){
            throw std::runtime_error(std::string("feature_recorder::write : feature_.size()=") + std::to_string(feature_.size()));
        }
        if (context_.size() > fs.opt_max_context_size){
            throw std::runtime_error(std::string("feature_recorder::write : context_.size()=") + std::to_string(context_.size()));
        }
    }

    /* TODO: This needs to be change to do all processing in utf32 and not utf8 */

    std::string feature      = feature_;
    std::string context      = flags.no_context ? "" : context_;
    std::string feature_utf8 = make_utf8(feature);

    quote_if_necessary(feature,context);

    if ( feature.size()==0 ){
        std::cerr << name << ": zero length feature at " << pos0 << "\n";
        if (fs.flags.pedantic) assert(0);
        return;
    }
    if ( fs.flags.pedantic ){
        /* Check for tabs or newlines in feature and and context */
        for(size_t i=0;i<feature.size();i++){
            if (feature[i]=='\t') throw std::runtime_error("feature contains \\t");
            if (feature[i]=='\n') throw std::runtime_error("feature contains \\n");
            if (feature[i]=='\r') throw std::runtime_error("feature contains \\r");
        }
        for(size_t i=0;i<context.size();i++){
            if (context[i]=='\t') throw std::runtime_error("context contains \\t");
            if (context[i]=='\n') throw std::runtime_error("context contains \\n");
            if (context[i]=='\r') throw std::runtime_error("context contains \\r");
        }
    }

    /* First check to see if the feature is on the stop list.
     * Only do this if we have a stop_list_recorder (the stop list recorder itself
     * does not have a stop list recorder. If it did we would infinitely recurse.
     */
    if (flags.no_stoplist==false
        && fs.stop_list
        && fs.stop_list_recorder
        && fs.stop_list->check_feature_context(feature_utf8,context)) {
        fs.stop_list_recorder->write(pos0,feature,context);
        return;
    }

    /* The alert list is a special features that are called out.
     * If we have one of those, write it to the redlist.
     */
#if 0
    if (flags.no_alertlist==false
        && fs.alert_list
        && fs.alert_list->check_feature_context(*feature_utf8,context)) {
        std::string alert_fn = fs.get_outdir() + "/ALERTS_found.txt";
        const std::lock_guard<std::mutex> lock(Mr);                // notice we are locking the alert list
        std::ofstream rf(alert_fn.c_str(),std::ios_base::app);
        if(rf.is_open()){
            rf << pos0.shift(fs.offset_add).str() << '\t' << feature << '\t' << "\n";
        }
    }
#endif

    /* add the feature to any histograms; the regex is applied in the histogram */
    this->histograms_add_feature(feature);

    /* Finally write out the feature and the context */
    this->write0(pos0, feature, context);
}

/**
 * Given a buffer, an offset into that buffer of the feature, and the length
 * of the feature, make the context and write it out. This is mostly used
 * for writing from within the lexical analyzers.
 */


/* for debugging, halt at this position */
const pos0_t debug_halt_pos0("",9999999);
const size_t debug_halt_pos(9999999);
void feature_recorder::write_buf(const sbuf_t &sbuf,size_t pos,size_t len)
{
    if (fs.flags.debug){
        std::cerr << "*** write_buf " << name << " sbuf=" << sbuf << " pos=" << pos << " len=" << len << "\n";
        /*
         * for debugging, you can set a debug at Breakpoint Reached below.
         * This lets you fast-forward to the error condition without having to set a watch point.
         */
        if (sbuf.pos0==debug_halt_pos0 || pos==debug_halt_pos) {
            std::cerr << "Breakpoint Reached.\n";
        }
    }

    /* If we are in the margin, ignore; it will be processed again */
    if(pos >= sbuf.pagesize && pos < sbuf.bufsize){
        return;
    }

    if(pos >= sbuf.bufsize){    /* Sanity checks */
        throw std::runtime_error(std::string("*** write_buf: WRITE OUTSIDE BUFFER. pos=")  + std::to_string(pos) + " sbuf=");
    }

    /* Asked to write beyond bufsize; bring it in */
    if (pos+len > sbuf.bufsize){
        len = sbuf.bufsize - pos;
    }

    std::string feature = sbuf.substr(pos,len);
    std::string context;

    if (flags.no_context==false) {
        /* Context write; create a clean context */
        size_t p0 = context_window < pos ? pos-context_window : 0;
        size_t p1 = pos+len+context_window;

        if (p1>sbuf.bufsize) p1 = sbuf.bufsize;
        assert( p0<=p1 );
        context = sbuf.substr(p0,p1-p0);
    }
    this->write(sbuf.pos0+pos, feature, context);
}


/**
 * replace a character in a string with another
 */
std::string replace(const std::string &src,char f,char t)
{
    std::string ret;
    for(size_t i=0;i<src.size();i++){
        if(src[i]==f) ret.push_back(t);
        else ret.push_back(src[i]);
    }
    return ret;
}

/****************************************************************
 *** carving support
 ****************************************************************
 *
 * carving support.
 * 2014-04-24 - $ is no longer valid either
 * 2013-08-29 - replace invalid characters in filenames
 * 2013-07-30 - automatically bin directories
 * 2013-06-08 - filenames are the forensic path.
 */

std::string valid_dosname(std::string in)
{
    std::string out;
    for(size_t i=0;i<in.size();i++){
        uint8_t ch = in.at(i);
        if(ch<=32 || ch>=128
           || ch=='"' || ch=='*' || ch=='+' || ch==','
           || ch=='/' || ch==':' || ch==';' || ch=='<'
           || ch=='=' || ch=='>' || ch=='?' || ch=='\\'
           || ch=='[' || ch==']' || ch=='|' || ch=='$' ){
            out.push_back('_');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

/*
 * write buffer to specified dirname/filename for writing data.
 * The name is {outdir}/{scanner}/{seq}/{pos0}.{ext}
 * Where: {outdir} is the output directory of the feature recorder.
 *        {scanner} is the name of the scanner
 *        {seq} is 000 through 999.  (1000 files per directory)
 *        {pos0} is where the feature was found.
 *        {ext} is the provided extension.
 */
std::string feature_recorder::carve_data(const sbuf_t &sbuf, const std::string &ext,
                                         const time_t mtime,
                                         const size_t offset,
                                         const size_t len)
{
    assert(offset < sbuf.bufsize);

    /* Carving to a file depends on the carving mode.
     *
     * The purpose of CARVE_ENCODED is to allow us to carve JPEGs when
     * they are embedded in, say, GZIP files, but not carve JPEGs that
     * are bare.  The difficulty arises when you have a tool that can
     * go into, say, ZIP files. In this case, we don't want to carve
     * every ZIP file, just the (for example) XORed ZIP files. So the
     * ZIP carver doesn't carve every ZIP file, just the ZIP files
     * that are in HIBER files.  That is, we want to not carve a path
     * of ZIP-234234 but we do want to carve a path of
     * 1000-HIBER-33423-ZIP-2343.  This is implemented by having an
     * do_not_carve_encoding. the ZIP carver sets it to ZIP so it
     * won't carve things that are just found in a ZIP file. This
     * means that it won't carve disembodied ZIP files found in
     * unallocated space. You might want to do that.  If so, set ZIP's
     * carve mode to CARVE_ALL.
     */
    switch(carve_mode){
    case CARVE_NONE:
        return NO_CARVED_FILE;                         // carve nothing
    case CARVE_ENCODED:
        if (sbuf.pos0.path.size() == 0 ) return NO_CARVED_FILE; // not encoded
        if (sbuf.pos0.alphaPart() == do_not_carve_encoding) return std::string(); // ignore if it is just encoded with this
        break;                                      // otherwise carve
    case CARVE_ALL:
        break;
    }

    /* Create the object that we are trying to carve */
    sbuf_t cbuf(sbuf, offset, len);

    /* See if we have previously carved this object, in which case do not carve it again */
    std::string carved_hash_hexvalue = hash(cbuf);
    if (carve_cache.check_for_presence_and_insert(carved_hash_hexvalue)){
        return NO_CARVED_FILE;
    }

    /* Determine the directory and filename */
    int64_t myfileNumber = carved_file_count++; // atomic operation
    std::ostringstream seq;

    seq << std::setw(3) << std::setfill('0') << int(myfileNumber/1000);

    const std::string scannerDir {fs.get_outdir() + "/" + name};
    const std::string seqDir {scannerDir + "/" + seq.str() };
    const std::string fname  {seqDir + "/" + sbuf.pos0.str() + "." + ext};

    /* Create the directory.
     * If it exists, the call fails.
     * (That's faster than checking to see if it exists and then creating the directory)
     */
    std::filesystem::create_directory( scannerDir );
    std::filesystem::create_directory( seqDir );

    /* Write the data */
    std::ofstream os;
    os.open( fname.c_str(), std::ios::out | std::ios::app | std::ios::binary | std::ios::trunc );
    if (!os.is_open()) {
        throw std::runtime_error("cannot open file "+fname);
    }
    else {
        os.write( reinterpret_cast<const char *>(cbuf.buf), cbuf.bufsize );
        if (os.bad()) {
            throw std::runtime_error("error writing file "+fname);
        }
        os.close();
    }

    /* Set timestamp if necessary */
    if (mtime>0) {
        const struct timeval times[2] = {{mtime,0},{mtime,0}};
        utimes(fname.c_str(),times);
    }

    return fname;
}

const std::string feature_recorder::hash(const sbuf_t &sbuf) const
{
    return (*fs.hasher.func)( reinterpret_cast<const uint8_t *>(sbuf.buf), sbuf.bufsize);
}


void feature_recorder::shutdown()
{
}

#include <iomanip>
#if 0
/**
 * @param sbuf   - the buffer to carve
 * @param pos    - offset in the buffer to carve
 * @param len    - how many bytes to carve
 *
 */
std::string feature_recorder::carve_data(const sbuf_t &sbuf, const std::string &ext,
                                         const time_t mtime, const size_t pos, const size_t len )
{
    if (fs.disabled) return std::string();           // disabled

    /* If we are in the margin, ignore; it will be processed again */
    if (pos >= sbuf.pagesize && pos < sbuf.bufsize){
        return std::string();
    }
    assert(pos < sbuf.bufsize);

    /* If the directory doesn't exist, make it.
     * If two threads try to make the directory,
     * that's okay, because the second one will fail.
     */

    sbuf_t cbuf(sbuf,pos,len);          // the buf we are going to carve
    std::string carved_hash_hexvalue = hash(cbuf);


    uint64_t this_file_number = file_number_add(in_cache ? 0 : 1); // increment if we are not in the cache
    std::string dirname1 = fs.get_outdir() + "/" + name;

    std::stringstream ss;
    ss << dirname1 << "/" << std::setw(3) << std::setfill('0') << (this_file_number / 1000);

    std::string dirname2 = ss.str();
    std::string fname         = dirname2 + std::string("/") + valid_dosname(cbuf.pos0.str() + ext);
    std::string fname_feature = fname.substr(fs.get_outdir().size()+1);

    /* Record what was found in the feature file.
     */
    if (in_cache){
        fname="";             // no filename
        fname_feature="<CACHED>";
    }

    // write to the feature file
    ss.str(std::string()); // clear the stringstream
    ss << "<fileobject>";
    if (!in_cache) ss << "<filename>" << fname << "</filename>";
    ss << "<filesize>" << len << "</filesize>";
    ss << "<hashdigest type='" << fs.hasher.name << "'>" << carved_hash_hexvalue << "</hashdigest></fileobject>";
    this->write(cbuf.pos0,fname_feature,ss.str());

    if (in_cache) return fname;               // do not make directories or write out if we are cached

    /* Make the directory if it doesn't exist.  */
    if (access(dirname2.c_str(),R_OK)!=0){
#ifdef WIN32
        mkdir(dirname1.c_str());
        mkdir(dirname2.c_str());
#else
        mkdir(dirname1.c_str(),0777);
        mkdir(dirname2.c_str(),0777);
#endif
    }
    /* Check to make sure that directory is there. We don't just the return code
     * because there could have been two attempts to make the directory simultaneously,
     * so the mkdir could fail but the directory could nevertheless exist. We need to
     * remember the error number because the access() call may clear it.
     */
    int oerrno = errno;                 // remember error number
    if (access(dirname2.c_str(),R_OK)!=0){
        std::cerr << "Could not make directory " << dirname2 << ": " << strerror(oerrno) << "\n";
        return std::string();
    }

    /* Write the file into the directory */
    int fd = ::open(fname.c_str(),O_CREAT|O_BINARY|O_RDWR,0666);
    if(fd<0){
        std::cerr << "*** carve: Cannot create " << fname << ": " << strerror(errno) << "\n";
        return std::string();
    }

    ssize_t ret = cbuf.write(fd,0,len);
    if(ret<0){
        std::cerr << "*** carve: Cannot write(pos=" << fd << "," << pos << " len=" << len << "): "<< strerror(errno) << "\n";
    }
    ::close(fd);
    return fname;
}
#endif

/*
 This is based on feature_recorder::carve and append carving record to specified filename
 */
std::string feature_recorder::carve_records(const sbuf_t &sbuf,
                                            size_t pos, size_t len, const std::string &filename)
{
#if 0
    if(flags & FLAG_DISABLED) return std::string();           // disabled

    if(pos >= sbuf.pagesize && pos < sbuf.bufsize){
        return std::string();
    }
    assert(pos < sbuf.bufsize);

    sbuf_t cbuf(sbuf,pos,len);          // the buf we are going to carve
    std::string carved_hash_hexvalue = (*fs.hasher.func)(cbuf.buf,cbuf.bufsize);

    /* See if this is in the cache */
    bool in_cache = carve_cache.check_for_presence_and_insert(carved_hash_hexvalue);
    std::string dirname1 = fs.get_outdir()  + "/" + name;

    std::stringstream ss;
    ss << dirname1;

    std::string dirname2 = ss.str();
    std::string fname = dirname2 + std::string("/") + valid_dosname(filename);
    std::string fname_feature = fname.substr(fs.get_outdir().size()+1);

    //    std::string fname = dirname2 + std::string("/") + valid_dosname(cbuf.pos0.str() + ext);
    //std::string fname_feature = fname.substr(fs.get_outdir().size()+1);

    /* Record what was found in the feature file.
     */
    if (in_cache){
        fname="";             // no filename
        fname_feature="<CACHED>";
    }

    // write to the feature file
    ss.str(std::string()); // clear the stringstream
    ss << len;
    this->write(cbuf.pos0,fname_feature,ss.str());

    if (in_cache) return fname;               // do not make directories or write out if we are cached

    /* Make the directory if it doesn't exist.  */
    if (access(dirname2.c_str(),R_OK)!=0){
#ifdef WIN32
        mkdir(dirname1.c_str());
        mkdir(dirname2.c_str());
#else
        mkdir(dirname1.c_str(),0777);
        mkdir(dirname2.c_str(),0777);
#endif
    }

    int oerrno = errno;                 // remember error number
    if (access(dirname2.c_str(),R_OK)!=0){
        std::cerr << "Could not make directory " << dirname2 << ": " << strerror(oerrno) << "\n";
        return std::string();
    }

    /* Write the file into the directory */
    int fd = ::open(fname.c_str(),O_APPEND|O_CREAT|O_BINARY|O_RDWR,0666);
    if(fd<0){
        std::cerr << "*** carve: Cannot create " << fname << ": " << strerror(errno) << "\n";
        return std::string();
    }

    ssize_t ret = cbuf.write(fd,0,len);
    if(ret<0){
        std::cerr << "*** carve: Cannot write(pos=" << fd << "," << pos << " len=" << len << "): "<< strerror(errno) << "\n";
    }
    ::close(fd);
    return fname;
#endif
    return std::string();
}

#if 0
/**
 * Currently, we need strptime() and utimes() to set the time.
 */
void feature_recorder::set_carve_mtime(const std::string &fname, const std::string &mtime_iso8601)
{
    if(flags & FLAG_DISABLED) return;           // disabled
#if defined(HAVE_STRPTIME) && defined(HAVE_UTIMES)
    if(fname.size()){
        struct tm tm;
        if(strptime(mtime_iso8601.c_str(),"%Y-%m-%dT%H:%M:%S",&tm)){
            time_t t = mktime(&tm);
            if(t>0){
                const struct timeval times[2] = {{t,0},{t,0}};
                utimes(fname.c_str(),times);
            }
        }
    }
#endif
}
#endif

/****************************************************************
 ** Histogram Support
 ****************************************************************/

/**
 * add a new histogram to this feature recorder.
 * @param def - the histogram definition
 */
void feature_recorder::histogram_add(const struct histogram_def &def)
{
    if (features_written != 0 ){
        throw std::runtime_error("Cannot add histograms after features have been written.");
    }
    histograms.push_back( std::make_unique<AtomicUnicodeHistogram>( def ));
}

/**
 * add a feature to all of the feature recorder's histograms
 * @param feature - the feature to add.
 */
void feature_recorder::histograms_add_feature(const std::string &feature)
{
    std::cerr << "add_feature('" << feature << "')\n";
    for (auto &h: histograms ){
        h->add(feature);               // add the original feature
    }
}

/**
 * flush the largest histogram to the disk. This is a way to release
 * allocated memory.
 *
 * In BE2.0, the file recorder's histograms are built in memory. If
 * they are too big for memory, file-histograms they are flushed and
 * need to be recombined in post-processing. SQL feature recorder uses the
 * SQLite3 to create the histograms.
 */

bool feature_recorder::histogram_flush_largest()
{
    /* TODO - implement */
    return false;
}

void feature_recorder::histogram_flush_all()
{
    for (auto &h: histograms ) {
        std::cerr << "histogram_flush \n";
        this->histogram_flush( *h );
    }
}

#if 0
/*
 * histogram_merge:
 * If possible, read all histogram files into memory and write them
 * out as a single histogram file. If we run out of memory during the process, fail.
 *
 */
void feature_recorder::histogram_merge(const struct histogram_def &def)
{
    throw std::runtime_error("Implement histogram_merge");
}

void feature_recorder::histogram_merge_all()
{
    throw std::runtime_error("Implement histogram_merge_all");
}
#endif
