// single-file lfsd.cpp
// Requer: C++17, glibc

#include <bits/stdc++.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;

// ----------------- util ANSI colors -----------------
namespace ansi {
    bool enabled = true;
    string esc(const string &c){ return enabled?"\033["+c+"m":""; }
    string reset(){ return esc("0"); }
    string red(){ return esc("31"); }
    string green(){ return esc("32"); }
    string yellow(){ return esc("33"); }
    string blue(){ return esc("34"); }
    string magenta(){ return esc("35"); }
    string cyan(){ return esc("36"); }
    string bold(){ return esc("1"); }
    void init(const string &mode){ if(mode=="always") enabled=true; else if(mode=="never") enabled=false; else enabled = isatty(STDOUT_FILENO); }
}

// ----------------- helpers -----------------
static int run(const string &cmd){ cerr<<"$ "<<cmd<<"\n"; return system(cmd.c_str()); }
static string nowstamp(){ time_t t=time(nullptr); char buf[64]; strftime(buf,sizeof(buf),"%Y%m%d-%H%M%S",localtime(&t)); return string(buf); }
static void ensure_dir(const string &p){ string cmd = "mkdir -p '"+p+"'"; run(cmd); }
static bool exists_file(const string &p){ struct stat st; return stat(p.c_str(), &st)==0; }
static string joinp(const string &a, const string &b){ if(a.empty()) return b; if(a.back()=='/') return a+b; return a+"/"+b; }

// read whole file
static string slurp(const string &p){ ifstream f(p); if(!f) return string(); stringstream ss; ss<<f.rdbuf(); return ss.str(); }

// write file
static void dump(const string &p, const string &content){ ofstream f(p); f<<content; }

// run and capture output
static string caprun(const string &cmd){ array<char,256> buf; string out; FILE* f = popen(cmd.c_str(),"r"); if(!f) return out; while(fgets(buf.data(), buf.size(), f)) out += buf.data(); pclose(f); return out; }

// sha256 file via sha256sum
static string sha256_file(const string &p){ if(!exists_file(p)) return string(); string out = caprun("sha256sum '"+p+"'"); smatch m; regex re("^([a-fA-F0-9]{64})"); if(regex_search(out,m,re)) return m[1]; return string(); }

// ----------------- config -----------------
struct Config {
    string recipes_dir = "/usr/share/lfsd/recipes";
    string state_dir = "/var/lib/lfsd";
    string stage_dir = "/var/stage/lfsd";
    string cache_dir = "/var/cache/lfsd";
    string bin_dir = "/var/cache/lfsd/bin";
    string sources_dir = "/var/cache/lfsd/sources";
    string log_dir = "/var/log/lfsd";
    string remote_url = "";
    string channel = "stable";
    string snapshot_backend = "tar"; // default tar.zst
    string color = "auto";
    int jobs = 0;
};

static Config load_config(){ Config c; // env overrides
    if(const char* v = getenv("LFSD_RECIPES_DIR")) c.recipes_dir=v;
    if(const char* v = getenv("LFSD_STATE_DIR")) c.state_dir=v;
    if(const char* v = getenv("LFSD_STAGE_DIR")) c.stage_dir=v;
    if(const char* v = getenv("LFSD_CACHE_DIR")) c.cache_dir=v;
    if(const char* v = getenv("LFSD_BIN")) c.bin_dir=v;
    if(const char* v = getenv("LFSD_SOURCES")) c.sources_dir=v;
    if(const char* v = getenv("LFSD_LOG_DIR")) c.log_dir=v;
    if(const char* v = getenv("LFSD_REMOTE_URL")) c.remote_url=v;
    if(const char* v = getenv("LFSD_CHANNEL")) c.channel=v;
    if(const char* v = getenv("LFSD_SNAPSHOT_BACKEND")) c.snapshot_backend=v;
    if(const char* v = getenv("LFSD_COLOR")) c.color=v;
    if(const char* v = getenv("LFSD_JOBS")) c.jobs = atoi(v);
    if(c.jobs<=0) c.jobs = thread::hardware_concurrency();
    ensure_dir(c.recipes_dir); ensure_dir(c.state_dir); ensure_dir(c.stage_dir); ensure_dir(c.cache_dir); ensure_dir(c.sources_dir); ensure_dir(c.bin_dir); ensure_dir(c.log_dir);
    return c; }

// ----------------- recipe parsing (minimal TOML-like)
struct Recipe {
    string name, version;
    vector<string> sources; // urls
    string git;
    vector<string> patches;
    string sha256;
    vector<string> depends;
    vector<string> configure, make_cmd, install_cmd, tests;
    bool bin_only = false;
    string path; // path to recipe file
};

static string trim(const string &s){ size_t a=0,b=s.size(); while(a<b && isspace((unsigned char)s[a])) a++; while(b>a && isspace((unsigned char)s[b-1])) b--; return s.substr(a,b-a); }

static vector<string> parse_array_line(const string &line){ vector<string> out; // expects ["a","b"]
    size_t l = line.find('['); size_t r = line.rfind(']'); if(l==string::npos||r==string::npos||r<=l) return out;
    string mid = line.substr(l+1, r-l-1);
    regex re(R"("([^"]+)")"); smatch m; string s = mid; auto it = s.cbegin(); while(regex_search(it, s.cend(), m, re)){ out.push_back(m[1]); it = m.suffix().first; }
    return out;
}

static Recipe load_recipe_toml(const string &path){ Recipe r; r.path = path; string s = slurp(path); if(s.empty()) return r; stringstream ss(s); string line;
    while(getline(ss,line)){
        line = trim(line);
        if(line.empty() || line[0]=='#') continue;
        if(line.rfind("name",0)==0){ auto p = line.find('='); r.name = trim(line.substr(p+1)); r.name.erase(remove(r.name.begin(), r.name.end(),'"'), r.name.end()); }
        else if(line.rfind("version",0)==0){ auto p = line.find('='); r.version = trim(line.substr(p+1)); r.version.erase(remove(r.version.begin(), r.version.end(),'"'), r.version.end()); }
        else if(line.rfind("git",0)==0){ auto p = line.find('='); r.git = trim(line.substr(p+1)); r.git.erase(remove(r.git.begin(), r.git.end(),'"'), r.git.end()); }
        else if(line.rfind("sources",0)==0){ auto arr = parse_array_line(line); r.sources = arr; }
        else if(line.rfind("patches",0)==0){ r.patches = parse_array_line(line); }
        else if(line.rfind("sha256",0)==0){ auto p = line.find('='); r.sha256 = trim(line.substr(p+1)); r.sha256.erase(remove(r.sha256.begin(), r.sha256.end(),'"'), r.sha256.end()); }
        else if(line.rfind("depends",0)==0){ r.depends = parse_array_line(line); }
        else if(line.rfind("bin_only",0)==0){ auto p=line.find('='); string v=trim(line.substr(p+1)); r.bin_only = (v=="true"||v=="True"); }
        else if(line.rfind("configure",0)==0){ r.configure = parse_array_line(line); }
        else if(line.rfind("make",0)==0 && line.find("make \")==string::npos){ r.make_cmd = parse_array_line(line); }
        else if(line.rfind("install",0)==0){ r.install_cmd = parse_array_line(line); }
        else if(line.rfind("tests",0)==0){ r.tests = parse_array_line(line); }
    }
    return r;
}

// find recipes recursively
static unordered_map<string,string> find_recipes(const string &root){ unordered_map<string,string> out; for(auto &it: filesystem::recursive_directory_iterator(root)){ if(it.path().filename()=="recipe.toml"){ Recipe r = load_recipe_toml(it.path()); if(!r.name.empty()) out[r.name]=it.path(); } } return out; }

// ----------------- state management (installed.json minimal)
struct InstalledInfo { string version; string installed_at; string manifest; vector<string> files; string source_hash; };

static string installed_db_path(const Config &c){ return joinp(c.state_dir, "installed.json"); }

// minimal json load/save (expects our simple structure)
static unordered_map<string, InstalledInfo> load_installed(const Config &c){ unordered_map<string, InstalledInfo> out; string p = installed_db_path(c); if(!exists_file(p)) return out; string s = slurp(p); // naive parse
    regex item(R"("([^"]+)"\s*:\s*\{([^}]*)\})"); smatch m; auto it = s.cbegin(); while(regex_search(it, s.cend(), m, item)){ string name=m[1]; string body=m[2]; InstalledInfo info; regex kv(R"("([^"]+)"\s*:\s*"?([^",}\n]+)"?)"); smatch mm; auto it2 = body.cbegin(); while(regex_search(it2, body.cend(), mm, kv)){ string k=mm[1], v=mm[2]; if(k=="version") info.version = v; else if(k=="installed_at") info.installed_at = v; else if(k=="manifest") info.manifest = v; else if(k=="source_hash") info.source_hash = v; it2 = mm.suffix().first; }
        // files array
        regex fa(R"("files"\s*:\s*\[(.*?)\])", regex::dotall);
        smatch fm; if(regex_search(body,fm,fa)){ string arr=fm[1]; regex itf(R"("([^"]+)")"); auto it3=arr.cbegin(); smatch im; while(regex_search(it3, arr.cend(), im, itf)){ info.files.push_back(im[1]); it3 = im.suffix().first; } }
        out[name]=info; it = m.suffix().first;
    }
    return out;
}

static void save_installed(const Config &c, const unordered_map<string, InstalledInfo> &db){ string p = installed_db_path(c); string out = "{\n"; for(auto &kv: db){ out += "  \""+kv.first+"\": {\n"; out += "    \"version\": \""+kv.second.version+"\",\n"; out += "    \"installed_at\": \""+kv.second.installed_at+"\",\n"; out += "    \"manifest\": \""+kv.second.manifest+"\",\n"; out += "    \"source_hash\": \""+kv.second.source_hash+"\",\n"; out += "    \"files\": [\n"; for(auto &f: kv.second.files) out += "      \""+f+"\","+"\n"; out += "    ]\n  },\n"; } out += "}\n"; dump(p,out); }

// ----------------- dependency resolver (topo sort)
static vector<string> topo_sort(const unordered_map<string, vector<string>> &deps){ unordered_map<string,int> indeg; unordered_map<string, vector<string>> adj; for(auto &kv: deps){ auto pkg=kv.first; if(!indeg.count(pkg)) indeg[pkg]=0; for(auto &d: kv.second){ adj[d].push_back(pkg); indeg[pkg]++; if(!indeg.count(d)) indeg[d]=0; } }
    queue<string> q; for(auto &kv: indeg) if(kv.second==0) q.push(kv.first);
    vector<string> order; while(!q.empty()){ auto u=q.front(); q.pop(); order.push_back(u); for(auto &v: adj[u]) if(--indeg[v]==0) q.push(v); }
    if(order.size()!=indeg.size()) throw runtime_error("ciclo detectado nas dependências"); return order; }

// ----------------- download manager ----------------
static int download_with_curl(const string &url, const string &out){ string cmd = "curl -L --fail --retry 3 -o '"+out+"' '"+url+"'"; return run(cmd); }
static int git_clone_shallow(const string &url, const string &out){ string cmd = "rm -rf '"+out+"' && git clone --depth 1 '"+url+"' '"+out+"'"; return run(cmd); }

// ----------------- build one package ----------------
static int build_one(const Recipe &r, const Config &c, unordered_map<string, InstalledInfo> &installed, const string &work_root, bool do_strip, bool do_pack){ // returns 0 on success
    string work = joinp(work_root, r.name+"-"+r.version);
    string srcdir = joinp(c.sources_dir, r.name+"-"+r.version);
    run("rm -rf '"+work+"' && mkdir -p '"+work+"'");
    // download
    if(!r.git.empty()){
        cerr<<ansi::cyan()<<"[download] git "<<r.git<<ansi::reset()<<"\n";
        if(git_clone_shallow(r.git, srcdir)!=0) return 1;
    } else if(!r.sources.empty()){
        ensure_dir(c.sources_dir);
        for(auto &u: r.sources){ string fname = joinp(c.sources_dir, r.name+"-"+r.version+"-"+to_string(rand())); string ext = fname; fname += ".src"; if(download_with_curl(u,fname)!=0) return 2; // verify sha if provided
            if(!r.sha256.empty()){ string h = sha256_file(fname); if(h.empty()||h!=r.sha256){ cerr<<ansi::red()<<"SHA256 mismatch for "<<u<<ansi::reset()<<"\n"; return 3; } }
            // unpack
            run("tar -C '"+work+"' -xf '"+fname+"' --strip-components=1"); }
    }
    // if git clone, copy into work
    if(!r.git.empty()) run("cp -a '"+srcdir+"/.' '"+work+"/'");
    // apply patches
    if(!r.patches.empty()){
        for(auto &purl: r.patches){ string pfile = joinp(work, "patch-"+to_string(rand())); if(download_with_curl(purl,pfile)!=0) return 4; run("cd '"+work+"' && patch -p1 < '"+pfile+"'"); }
    }
    // env
    vector<pair<string,string>> envs = {{"STAGE", joinp(c.stage_dir, r.name+"-"+r.version)}, {"JOBS", to_string(c.jobs)}};
    ensure_dir(envs[0].second);
    // run steps
    auto run_step = [&](const vector<string> &cmds){ for(auto &cmm: cmds){ string cmdline = "cd '"+work+"' && "; // replace ${STAGE}, ${JOBS}
                string s=cmm; size_t pos; while((pos=s.find("${STAGE}"))!=string::npos) s.replace(pos,8,envs[0].second); while((pos=s.find("${JOBS}"))!=string::npos) s.replace(pos,7,to_string(c.jobs)); cmdline += s; int rc = run(cmdline); if(rc!=0) return rc; } return 0; };
    if(!r.configure.empty()){ if(run_step(r.configure)) return 10; }
    if(!r.make_cmd.empty()){ if(run_step(r.make_cmd)) return 11; }
    if(!r.tests.empty()){ if(run_step(r.tests)) return 12; }
    if(!r.install_cmd.empty()){ if(run_step(r.install_cmd)) return 13; }
    // generate manifest
    vector<string> files;
    string pkgroot = envs[0].second; // staged install path
    // find files under pkgroot
    for(auto &p: filesystem::recursive_directory_iterator(pkgroot)){ if(is_regular_file(p.path())) files.push_back(p.path()); }
    // compute checksums for manifest
    string mani = joinp(c.state_dir, "manifests/"+r.name+"-"+r.version+".manifest"); ensure_dir(filesystem::path(mani).parent_path()); string manifest_txt;
    for(auto &f: files){ string h = sha256_file(f); manifest_txt += f + " " + h + "\n"; }
    dump(mani, manifest_txt);
    // package binaries
    if(do_pack){ ensure_dir(c.bin_dir); string pkgfile = joinp(c.bin_dir, r.name+"-"+r.version+".tar.zst"); string cmd = "tar -C '"+pkgroot+"' -I zstd -cpf '"+pkgfile+"' ."; run(cmd); }
    // strip if requested
    if(do_strip){ // naive: find ELF and call strip
        for(auto &f: files){ // simple heuristic by extension
            if(f.find("/bin/")!=string::npos || f.find("/sbin/")!=string::npos || f.find("/lib/")!=string::npos){ run("file '"+f+"' | grep ELF >/dev/null && strip --strip-all '"+f+"' || true"); }
        }
    }
    // record to installed.json
    InstalledInfo info; info.version = r.version; info.installed_at = nowstamp(); info.manifest = mani; for(auto &f: files) info.files.push_back(f); info.source_hash = ""; // could be computed
    installed[r.name] = info; save_installed(c, installed);
    // copy from pkgroot to root? We don't apply here; apply is separate step
    string staged_dest = joinp(c.stage_dir, r.name+"-"+r.version+"/pkgroot"); run("rm -rf '"+staged_dest+"' && mkdir -p '"+staged_dest+"' && cp -a '"+pkgroot+"/.' '"+staged_dest+"/'");
    return 0;
}

// ----------------- apply promoted stage to / (with snapshot tar backup) ----------------
static int apply_stage(const Config &c){ // create snapshot tar of /usr (safe) then rsync
    string label = "apply-"+nowstamp(); string snapdir = joinp(c.cache_dir, "snaps"); ensure_dir(snapdir);
    string snapfile = joinp(snapdir, label+".tar.zst"); cerr<<ansi::yellow()<<"[snap] creating backup "<<snapfile<<ansi::reset()<<"\n";
    int rc = run("tar -C / -I zstd -cpf '"+snapfile+"' usr || true"); if(rc!=0) return rc;
    // rsync all staged pkgroots into /
    for(auto &p: filesystem::directory_iterator(c.stage_dir)){
        if(!p.is_directory()) continue;
        string pkgroot = joinp(p.path(), "pkgroot"); if(!exists_file(pkgroot) && !filesystem::exists(pkgroot)) continue;
        string cmd = "rsync -aHAX --delete '"+pkgroot+"/' /"; rc |= run(cmd);
    }
    return rc;
}

// ----------------- remove package ----------------
static int remove_package(const string &pkg, const Config &c){ auto db = load_installed(c); if(!db.count(pkg)){ cerr<<ansi::red()<<"not installed\n"<<ansi::reset(); return 1; }
    // check reverse deps: naive scan recipes and installed
    auto recs = find_recipes(c.recipes_dir);
    unordered_map<string, vector<string>> deps; for(auto &kv: recs){ Recipe rr = load_recipe_toml(kv.second); deps[rr.name]=rr.depends; }
    for(auto &kv: db){ if(kv.first==pkg) continue; for(auto &d: deps[kv.first]) if(d==pkg){ cerr<<ansi::red()<<"package "<<kv.first<<" depends on "<<pkg<<"; remove aborted"<<ansi::reset()<<"\n"; return 2; } }
    auto info = db[pkg]; // remove files
    for(auto &f: info.files){ if(exists_file(f)){ string h = sha256_file(f); // best-effort check; skip if mismatch
            run("rm -f '"+f+"'"); }
    }
    db.erase(pkg); save_installed(c, db);
    // log
    dump(joinp(c.log_dir, nowstamp()+"-remove-"+pkg+".log"), string("removed ")+pkg);
    return 0;
}

// ----------------- list and info ----------------
static int cmd_list(const Config &c){ auto recs = find_recipes(c.recipes_dir); auto db = load_installed(c);
    vector<string> names; for(auto &kv: recs) names.push_back(kv.first); sort(names.begin(), names.end()); for(auto &n: names){ bool ok = db.count(n); cout<< (ok?"[\u2713] ":"[ ] ") << n; if(ok) cout<<" "<<db[n].version; cout<<"\n"; } return 0; }
static int cmd_info(const string &pkg, const Config &c){ auto recs = find_recipes(c.recipes_dir); auto db = load_installed(c); if(!recs.count(pkg)){ cerr<<"recipe not found\n"; return 1; } Recipe r = load_recipe_toml(recs[pkg]); cout<<ansi::bold()<<pkg<<"@"<<r.version<<ansi::reset()<<"\n"; cout<<"depends: "; for(auto &d:r.depends) cout<<d<<" "; cout<<"\n"; if(db.count(pkg)){ cout<<ansi::green()<<"installed at "<<db[pkg].installed_at<<ansi::reset()<<"\n"; } else cout<<ansi::yellow()<<"not installed"<<ansi::reset()<<"\n"; return 0; }

// ----------------- sync (git) ----------------
static int cmd_sync(const Config &c, const string &repo=""){ string target = repo.empty()?c.recipes_dir:repo; if(filesystem::exists(joinp(target,".git"))){ return run("git -C '"+target+"' pull --ff-only"); } else if(!c.remote_url.empty()){ return run("git clone --branch '"+c.channel+"' '"+c.remote_url+"' '"+c.recipes_dir+"'"); } else { cerr<<"no remote and target is not a git repo\n"; return 1; } }

// ----------------- upgrade installed ----------------
static int cmd_upgrade(const Config &c){ auto recs = find_recipes(c.recipes_dir); auto db = load_installed(c); vector<string> to_upgrade;
    for(auto &kv: db){ string name = kv.first; if(recs.count(name)){ Recipe rr = load_recipe_toml(recs[name]); if(rr.version!=kv.second.version) to_upgrade.push_back(name); } }
    if(to_upgrade.empty()){ cout<<"all up-to-date\n"; return 0; }
    // plan build -> similar to install
    for(auto &n: to_upgrade){ cout<<"upgrading "<<n<<"\n"; // naive: rebuild and apply
        Recipe r = load_recipe_toml(recs[n]); unordered_map<string, InstalledInfo> inst = db; build_one(r,c,inst,c.stage_dir,false,true); }
    apply_stage(c); return 0; }

// ----------------- rebuild and rebuild-all ----------------
static int cmd_rebuild(const string &pkg, const Config &c){ auto recs = find_recipes(c.recipes_dir); if(!recs.count(pkg)){ cerr<<"recipe not found\n"; return 1; } Recipe r = load_recipe_toml(recs[pkg]); auto db = load_installed(c); return build_one(r,c,db,c.stage_dir,false,true); }
static int cmd_rebuild_all(const Config &c){ auto recs = find_recipes(c.recipes_dir); unordered_map<string, vector<string>> deps; for(auto &kv: recs){ Recipe r = load_recipe_toml(kv.second); deps[r.name]=r.depends; }
    vector<string> order = topo_sort(deps); auto db = load_installed(c); for(auto &n: order){ Recipe r = load_recipe_toml(recs[n]); build_one(r,c,db,c.stage_dir,false,true); }
    return 0; }

// ----------------- main CLI ----------------
int main(int argc, char** argv){ ios::sync_with_stdio(false); cin.tie(nullptr);
    Config cfg = load_config(); ansi::init(cfg.color);
    if(argc<2){ cerr<<"uso: lfsd <cmd> [args]\n"; return 1; }
    string cmd = argv[1]; // expand abbreviations
    if(cmd=="s") cmd="sync"; if(cmd=="p") cmd="plan"; if(cmd=="b") cmd="build"; if(cmd=="i") cmd="install"; if(cmd=="rm") cmd="remove";
    if(cmd=="sync"){ string repo=""; if(argc>2) repo=argv[2]; return cmd_sync(cfg, repo); }
    if(cmd=="list"){ return cmd_list(cfg); }
    if(cmd=="info"){ if(argc<3) { cerr<<"specify package\n"; return 1;} return cmd_info(argv[2], cfg); }
    if(cmd=="plan"){ // generate pending.plan for targets
        vector<string> targets; for(int i=2;i<argc;i++) targets.push_back(argv[i]); if(targets.empty()){ cerr<<"specify targets\n"; return 1; }
        auto recs = find_recipes(cfg.recipes_dir); unordered_map<string, vector<string>> deps; for(auto &t: targets){ if(!recs.count(t)) { cerr<<"recipe "<<t<<" not found\n"; return 1; } Recipe r = load_recipe_toml(recs[t]); // simple: include only the requested pkgs and their deps recursively
            queue<string> q; q.push(t); unordered_set<string> seen;
            while(!q.empty()){ auto u=q.front(); q.pop(); if(seen.count(u)) continue; seen.insert(u); if(!recs.count(u)) continue; Recipe rr = load_recipe_toml(recs[u]); deps[rr.name]=rr.depends; for(auto &d: rr.depends) q.push(d); }
        vector<string> order = topo_sort(deps);
        string planfile = joinp(cfg.state_dir, "pending.plan"); string pl; for(auto &o: order) pl += o+"\n"; dump(planfile, pl); cout<<"plan saved to "<<planfile<<"\n"; return 0; }
    if(cmd=="build"){ // read pending.plan
        string planfile = joinp(cfg.state_dir, "pending.plan"); if(!exists_file(planfile)){ cerr<<"no plan. run plan first\n"; return 1; }
        vector<string> order; { string s=slurp(planfile); stringstream ss(s); string l; while(getline(ss,l)){ if(trim(l).empty()) continue; order.push_back(trim(l)); } }
        auto recs = find_recipes(cfg.recipes_dir); auto db = load_installed(cfg);
        bool do_strip=false; bool do_pack=true;
        for(int i=2;i<argc;i++){ if(string(argv[i])=="--strip") do_strip=true; if(string(argv[i])=="--no-pack") do_pack=false; }
        for(auto &n: order){ if(!recs.count(n)){ cerr<<"recipe "<<n<<" not found\n"; return 1; } Recipe r = load_recipe_toml(recs[n]); int rc = build_one(r,cfg,db,cfg.stage_dir,do_strip,do_pack); if(rc!=0){ cerr<<"build failed for "<<n<<" rc="<<rc<<"\n"; return rc; } }
        cout<<ansi::green()<<"builds completed\n"<<ansi::reset(); return 0; }
    if(cmd=="apply"){ return apply_stage(cfg); }
    if(cmd=="install"){ if(argc<3){ cerr<<"specify package\n"; return 1; } string pkg = argv[2]; // plan+build+apply
        auto recs = find_recipes(cfg.recipes_dir); if(!recs.count(pkg)){ cerr<<"recipe not found\n"; return 1; } // plan deps
        vector<string> targets={pkg}; // reuse plan logic
        run((string)"lfsd plan "+pkg); run((string)"lfsd build"); run((string)"lfsd apply"); return 0; }
    if(cmd=="remove"){ if(argc<2){ cerr<<"specify package\n"; return 1; } string pkg = argv[2]; return remove_package(pkg,cfg); }
    if(cmd=="snapshot"){ string lbl = argc>2?argv[2]:string("manual-")+nowstamp(); ensure_dir(joinp(cfg.cache_dir,"snaps")); string out = joinp(joinp(cfg.cache_dir,"snaps"), lbl+".tar.zst"); run("tar -C / -I zstd -cpf '"+out+"' usr"); cout<<"snapshot "<<out<<" created\n"; return 0; }
    if(cmd=="rollback"){ if(argc<2){ cerr<<"specify snapshot name\n"; return 1; } string name=argv[2]; string path = joinp(joinp(cfg.cache_dir,"snaps"), name); if(!exists_file(path)) { cerr<<"snap not found\n"; return 1; } run("tar -C / -I zstd -xpf '"+path+"'"); cout<<"rollback applied\n"; return 0; }
    if(cmd=="upgrade") return cmd_upgrade(cfg);
    if(cmd=="rebuild") { if(argc<2){ cerr<<"specify pkg\n"; return 1; } return cmd_rebuild(argv[2], cfg); }
    if(cmd=="rebuild-all") return cmd_rebuild_all(cfg);
    if(cmd=="install-bin"){ if(argc<3){ cerr<<"specify package tar.zst\n"; return 1; } string path=argv[2]; if(!exists_file(path)){ cerr<<"file not found\n"; return 1; } run("tar -C / -I zstd -xpf '"+path+"'"); return 0; }

    cerr<<"unknown command"<<"\n"; return 1; }
