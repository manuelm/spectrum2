#include "transport/config.h"
#include "transport/transport.h"
#include "transport/filetransfermanager.h"
#include "transport/usermanager.h"
#include "transport/logger.h"
#include "transport/sqlite3backend.h"
#include "transport/mysqlbackend.h"
#include "transport/userregistration.h"
#include "transport/networkpluginserver.h"
#include "transport/admininterface.h"
#include "transport/util.h"
#include "Swiften/EventLoop/SimpleEventLoop.h"
#include <boost/filesystem.hpp>
#ifndef WIN32
#include "sys/signal.h"
#include <pwd.h>
#include <grp.h>
#include <sys/resource.h>
#include "libgen.h"
#else
#include <windows.h>
#endif
#include "log4cxx/logger.h"
#include "log4cxx/consoleappender.h"
#include "log4cxx/patternlayout.h"
#include "log4cxx/propertyconfigurator.h"
#include "log4cxx/helpers/properties.h"
#include "log4cxx/helpers/transcoder.h"
#include "log4cxx/helpers/fileinputstream.h"
#include <sys/stat.h>

using namespace log4cxx;

using namespace Transport;

static LoggerPtr logger = log4cxx::Logger::getLogger("Spectrum");

Swift::SimpleEventLoop *eventLoop_ = NULL;
Component *component_ = NULL;
UserManager *userManager_ = NULL;

static void stop_spectrum() {
	userManager_->removeAllUsers();
	component_->stop();
	eventLoop_->stop();
}

static void spectrum_sigint_handler(int sig) {
	eventLoop_->postEvent(&stop_spectrum);
}

static void spectrum_sigterm_handler(int sig) {
	eventLoop_->postEvent(&stop_spectrum);
}

static void removeOldIcons(std::string iconDir) {
	std::vector<std::string> dirs;
	dirs.push_back(iconDir);

	boost::thread thread(boost::bind(Util::removeEverythingOlderThan, dirs, time(NULL) - 3600*24*14));
}

#ifndef WIN32
static void daemonize(const char *cwd, const char *lock_file) {
	pid_t pid, sid;
	FILE* lock_file_f;
	char process_pid[20];
	/* already a daemon */
	if ( getppid() == 1 ) return;

	/* Fork off the parent process */
	pid = fork();
	if (pid < 0) {
		exit(1);
	}
	/* If we got a good PID, then we can exit the parent process. */
	if (pid > 0) {
		if (lock_file) {
			/* write our pid into it & close the file. */
			lock_file_f = fopen(lock_file, "w+");
			if (lock_file_f == NULL) {
				std::cerr << "Cannot create lock file " << lock_file << ". Exiting\n";
				exit(1);
			}
			sprintf(process_pid,"%d\n",pid);
			if (fwrite(process_pid,1,strlen(process_pid),lock_file_f) < strlen(process_pid)) {
				std::cerr << "Cannot write to lock file " << lock_file << ". Exiting\n";
				exit(1);
			}
			fclose(lock_file_f);
		}
		exit(0);
	}

	/* Change the file mode mask */
	umask(0);

	/* Create a new SID for the child process */
	sid = setsid();
	if (sid < 0) {
		exit(1);
	}

	/* Change the current working directory.  This prevents the current
		directory from being locked; hence not being able to remove it. */
	if ((chdir(cwd)) < 0) {
		exit(1);
	}
	
	if (freopen( "/dev/null", "r", stdin) == NULL) {
		std::cout << "EE cannot open /dev/null. Exiting\n";
		exit(1);
	}
}

#endif

int main(int argc, char **argv)
{
	Config config;

	boost::program_options::variables_map vm;
	bool no_daemon = false;
	std::string config_file;
	std::string jid;
	

#ifndef WIN32
	if (signal(SIGINT, spectrum_sigint_handler) == SIG_ERR) {
		std::cout << "SIGINT handler can't be set\n";
		return -1;
	}

	if (signal(SIGTERM, spectrum_sigterm_handler) == SIG_ERR) {
		std::cout << "SIGTERM handler can't be set\n";
		return -1;
	}
#endif
	boost::program_options::options_description desc("Usage: spectrum [OPTIONS] <config_file.cfg>\nAllowed options");
	desc.add_options()
		("help,h", "help")
		("no-daemonize,n", "Do not run spectrum as daemon")
		("no-debug,d", "Create coredumps on crash")
		("jid,j", boost::program_options::value<std::string>(&jid)->default_value(""), "Specify JID of transport manually")
		("config", boost::program_options::value<std::string>(&config_file)->default_value(""), "Config file")
		;
	try
	{
		boost::program_options::positional_options_description p;
		p.add("config", -1);
		boost::program_options::store(boost::program_options::command_line_parser(argc, argv).
          options(desc).positional(p).run(), vm);
		boost::program_options::notify(vm);

		

		if(vm.count("help"))
		{
			std::cout << desc << "\n";
			return 1;
		}

		if(vm.count("config") == 0) {
			std::cout << desc << "\n";
			return 1;
		}

		if(vm.count("no-daemonize")) {
			no_daemon = true;
		}
	}
	catch (std::runtime_error& e)
	{
		std::cout << desc << "\n";
		return 1;
	}
	catch (...)
	{
		std::cout << desc << "\n";
		return 1;
	}

	if (!config.load(vm["config"].as<std::string>(), jid)) {
		std::cerr << "Can't load configuration file.\n";
		return 1;
	}

#ifndef WIN32
	if (!no_daemon) {
		// create directories
		try {
			boost::filesystem::create_directories(
				boost::filesystem::path(CONFIG_STRING(&config, "service.pidfile")).parent_path().string()
			);
		}
		catch (...) {
			std::cerr << "Can't create service.pidfile directory " << boost::filesystem::path(CONFIG_STRING(&config, "service.pidfile")).parent_path().string() << ".\n";
			return 1;
		}

		// daemonize
		daemonize(CONFIG_STRING(&config, "service.working_dir").c_str(), CONFIG_STRING(&config, "service.pidfile").c_str());
// 		removeOldIcons(CONFIG_STRING(&config, "service.working_dir") + "/icons");
    }
#endif

	if (CONFIG_STRING(&config, "logging.config").empty()) {
		LoggerPtr root = log4cxx::Logger::getRootLogger();
#ifdef WIN32
		root->addAppender(new ConsoleAppender(new PatternLayout(L"%d %-5p %c: %m%n")));
#else
		root->addAppender(new ConsoleAppender(new PatternLayout("%d %-5p %c: %m%n")));
#endif
	}
	else {
		log4cxx::helpers::Properties p;
		log4cxx::helpers::FileInputStream *istream = new log4cxx::helpers::FileInputStream(CONFIG_STRING(&config, "logging.config"));

		p.load(istream);
		LogString pid, jid;
		log4cxx::helpers::Transcoder::decode(boost::lexical_cast<std::string>(getpid()), pid);
		log4cxx::helpers::Transcoder::decode(CONFIG_STRING(&config, "service.jid"), jid);
#ifdef WIN32
		p.setProperty(L"pid", pid);
		p.setProperty(L"jid", jid);
#else
		p.setProperty("pid", pid);
		p.setProperty("jid", jid);
#endif
		log4cxx::PropertyConfigurator::configure(p);
	}

#ifndef WIN32
	if (!CONFIG_STRING(&config, "service.group").empty() ||!CONFIG_STRING(&config, "service.user").empty() ) {
		struct rlimit limit;
		getrlimit(RLIMIT_CORE, &limit);

		if (!CONFIG_STRING(&config, "service.group").empty()) {
			struct group *gr;
			if ((gr = getgrnam(CONFIG_STRING(&config, "service.group").c_str())) == NULL) {
				LOG4CXX_ERROR(logger, "Invalid service.group name " << CONFIG_STRING(&config, "service.group"));
				return 1;
			}

			if (((setgid(gr->gr_gid)) != 0) || (initgroups(CONFIG_STRING(&config, "service.user").c_str(), gr->gr_gid) != 0)) {
				LOG4CXX_ERROR(logger, "Failed to set service.group name " << CONFIG_STRING(&config, "service.group") << " - " << gr->gr_gid << ":" << strerror(errno));
				return 1;
			}
		}

		if (!CONFIG_STRING(&config, "service.user").empty()) {
			struct passwd *pw;
			if ((pw = getpwnam(CONFIG_STRING(&config, "service.user").c_str())) == NULL) {
				LOG4CXX_ERROR(logger, "Invalid service.user name " << CONFIG_STRING(&config, "service.user"));
				return 1;
			}

			if ((setuid(pw->pw_uid)) != 0) {
				LOG4CXX_ERROR(logger, "Failed to set service.user name " << CONFIG_STRING(&config, "service.user") << " - " << pw->pw_uid << ":" << strerror(errno));
				return 1;
			}
		}
		setrlimit(RLIMIT_CORE, &limit);
	}

	struct rlimit limit;
	limit.rlim_max = RLIM_INFINITY;
	limit.rlim_cur = RLIM_INFINITY;
	setrlimit(RLIMIT_CORE, &limit);

	// create directories
	try {
		boost::filesystem::create_directories(CONFIG_STRING(&config, "service.working_dir"));
	}
	catch (...) {
		std::cerr << "Can't create service.working_dir directory " << CONFIG_STRING(&config, "service.working_dir") << ".\n";
		return 1;
	}
#endif

	Swift::SimpleEventLoop eventLoop;

	Swift::BoostNetworkFactories *factories = new Swift::BoostNetworkFactories(&eventLoop);
	UserRegistry userRegistry(&config, factories);

	Component transport(&eventLoop, factories, &config, NULL, &userRegistry);
	component_ = &transport;
// 	Logger logger(&transport);

	StorageBackend *storageBackend = NULL;

#ifdef WITH_SQLITE
	if (CONFIG_STRING(&config, "database.type") == "sqlite3") {
		storageBackend = new SQLite3Backend(&config);
		if (!storageBackend->connect()) {
			std::cerr << "Can't connect to database.\n";
			return -1;
		}
	}
#endif
#ifdef WITH_MYSQL
	if (CONFIG_STRING(&config, "database.type") == "mysql") {
		storageBackend = new MySQLBackend(&config);
		if (!storageBackend->connect()) {
			std::cerr << "Can't connect to database.\n";
			return -1;
		}
	}
#endif

	UserManager userManager(&transport, &userRegistry, storageBackend);
	userManager_ = &userManager;
	UserRegistration *userRegistration = NULL;
	if (storageBackend) {
		userRegistration = new UserRegistration(&transport, &userManager, storageBackend);
		userRegistration->start();
// 		logger.setUserRegistration(&userRegistration);
	}
// 	logger.setUserManager(&userManager);

	FileTransferManager ftManager(&transport, &userManager);

	NetworkPluginServer plugin(&transport, &config, &userManager, &ftManager);

	AdminInterface adminInterface(&transport, &userManager, &plugin, storageBackend);

	eventLoop_ = &eventLoop;

	eventLoop.run();

	if (userRegistration) {
		userRegistration->stop();
		delete userRegistration;
	}
	delete storageBackend;
	delete factories;
}
