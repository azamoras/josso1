
#include <JOSSOIsapiAgent/agent/AbstractSSOAgent.hpp>
#define _CRTDBG_MAP_ALLOC
#include <iostream>
#include <crtdbg.h>

#include "JOSSOIsapiAgent/agent/wsclient/SSOIdentityManagerSOAPBinding.nsmap"
#include "JOSSOIsapiAgent/agent/wsclient/soapSSOIdentityProviderSOAPBindingProxy.h"
#include "JOSSOIsapiAgent/agent/wsclient/soapSSOIdentityManagerSOAPBindingProxy.h"
#include "JOSSOIsapiAgent/agent/wsclient/soapSSOSessionManagerSOAPBindingProxy.h"

#include "JOSSOIsapiAgent/util/simpleini/SimpleIni.h"
#include "JOSSOIsapiAgent/util/StringUtil.hpp"

#include <JOSSOIsapiAgent/agent/autologin/AbstractAutomaticLoginStrategy.hpp>

#include <regex.h>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif



using namespace std;


// Initialize some constants

const char *AbstractSSOAgent::JOSSO_REGISTRY_LOCATION = "Software\\Atricore\\JOSSO Isapi Agent\\1.8";

const char *AbstractSSOAgent::JOSSO_LOG_FILE = "LogFile";

const char *AbstractSSOAgent::JOSSO_LOG_LEVEL = "LogLevel";

const char *AbstractSSOAgent::JOSSO_AGENT_CONFIG = "AgentConfigFile";

const char *AbstractSSOAgent::JOSSO_ERROR_PAGE = "ErrorPage";

const char *AbstractSSOAgent::JOSSO_EXTENSION_URI = "ExtensionUri";

const char *AbstractSSOAgent::W3SVC_REGISTRY_KEY = "SYSTEM\\CurrentControlSet\\Services\\W3SVC\\Parameters";

const long AbstractSSOAgent::DEFAULT_SESSION_ACCESS_MIN_INTERVAL = 1000;

const long AbstractSSOAgent::DEFAULT_CACHE_CLEANUP_MIN_INTERVAL = 60000;

const char *AbstractSSOAgent::DEFAULT_SESSION_MANAGER_SERVICE_PATH = "/josso/services/SSOSessionManagerSoap";

const char *AbstractSSOAgent::DEFAULT_IDENTITY_MANAGER_SERVICE_PATH = "/josso/services/SSOIdentityManagerSoap";

const char *AbstractSSOAgent::DEFAULT_IDENTITY_PROVIDER_SERVICE_PATH = "/josso/services/SSOIdentityProviderSoap";

const char *AbstractSSOAgent::JOSSO_DEFAULT_AUTH_LOGIN_STRATEGY = "DEFAULT";

const char *AbstractSSOAgent::JOSSO_URLBASED_AUTH_LOGIN_STRATEGY = "URLBASED";

const char *AbstractSSOAgent::JOSSO_BOT_AUTH_LOGIN_STRATEGY = "BOT";

const char *AbstractSSOAgent::JOSSO_AUTH_LOGIN_REQUIRED = "REQUIRED";

const char *AbstractSSOAgent::JOSSO_AUTH_LOGIN_SUFFICIENT = "SUFFICIENT";

const char *AbstractSSOAgent::JOSSO_AUTH_LOGIN_OPTIONAL = "OPTIONAL";

const ULONGLONG nano100SecInMilliSec = (ULONGLONG)10000;

jk_logger_t *AbstractSSOAgent::logger = NULL;

map<string, FILETIME> cache;

// This is used as a mutex object in multi threading environments.
CRITICAL_SECTION cacheMapLock;

FILETIME lastCacheCleanupTime;

// -------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------

/**
 * Start this agent
 **/
bool AbstractSSOAgent::start() {

	bool ok = true;

	syslog(JK_LOG_INFO_LEVEL, "Starting JOSSO C Agent [%s]", (VERSION_STRING));

	agentConfig = createAgentConfig();

	configureAgent(agentConfig);

	// Start logging services !

	if (logger == NULL) {

		syslog(JK_LOG_DEBUG_LEVEL, "Opening log at %s", agentConfig->logFile);
		if (!jk_open_file_logger(&logger, agentConfig->logFile, agentConfig->logLevel)) {
			logger = NULL;
			syslog(JK_LOG_WARNING_LEVEL, "Cannot open log at %s, verify folder and file permissions. JOSSO Logging will be disabled." , agentConfig->logFile);
			ok = false;
		} else {
			syslog(JK_LOG_INFO_LEVEL, "Opening log at %s ... OK" , agentConfig->logFile);
		}
	}

	if (ok) {
		jk_log(logger, JK_LOG_INFO, "Started JOSSO C Agent %s", (VERSION_STRING));

		InitializeCriticalSection(&cacheMapLock);
		GetSystemTimeAsFileTime(&lastCacheCleanupTime);
		
		// Dump configuration if DEBUG is enabled:

		if (JK_IS_DEBUG_LEVEL(logger)) {

			jk_log(logger, JK_LOG_DEBUG, "getGatewayEndpoint:%s", agentConfig->getGatewayEndpoint());
			jk_log(logger, JK_LOG_DEBUG, "getGatewayLoginUrl:%s", agentConfig->getGatewayLoginUrl());

			// Dump partner apps 
			
			list<PartnerAppConfig>::const_iterator partnerApp;

			for (partnerApp = agentConfig->apps.begin(); partnerApp != agentConfig->apps.end() ; partnerApp++ ) { 
				jk_log(logger, JK_LOG_DEBUG, "partnerApp:%s at base-uri:%s", partnerApp->id.c_str(), partnerApp->baseUri.c_str());
			}

			// Dump security constraints
			list<SecurityConstraintConfig>::const_iterator secConstraint;
			for ( secConstraint = agentConfig->secConstraints.begin() ; secConstraint != agentConfig->secConstraints.end() ; secConstraint++) {

				jk_log(logger, JK_LOG_DEBUG, "securityConstraint:%s has %d roles", secConstraint->id.c_str(), secConstraint->roles.size());
				vector<string>::const_iterator baseUri;
				for(baseUri = secConstraint->baseUris.begin() ; baseUri != secConstraint->baseUris.end() ; baseUri ++) {
					jk_log(logger, JK_LOG_DEBUG, "    (%s) baseUri:%s", secConstraint->id.c_str(), baseUri->c_str());
				}

				vector<string>::const_iterator role;
				for (role = secConstraint->roles.begin() ; role < secConstraint->roles.end() ; role++) {
					jk_log(logger, JK_LOG_DEBUG, "    (%s) role:%s", secConstraint->id.c_str(), role->c_str());
				}
			}

			

			// agentConfig->


		}
	} else {
		syslog(JK_LOG_ERROR_LEVEL, "Failed to start JOSSO C Agent %s", (VERSION_STRING));
	}

	started = ok;

	return ok;

}

bool AbstractSSOAgent::configureAgent(AgentConfig *cfg) {

	bool ok = true;


	bool            a_bIsUtf8 = false;
    bool            a_bUseMultiKey = true;
    bool            a_bUseMultiLine	= true;

	// Agent config INI file
	CSimpleIniA ini(a_bIsUtf8, a_bUseMultiKey, a_bUseMultiLine);
	SI_Error rc = ini.LoadFile(cfg->agentConfigFile);
	if (rc < 0) {
		syslog(JK_LOG_ERROR_LEVEL, "Cannot read agent configuartion file %s", cfg->agentConfigFile);
		return false;
	}

	// ----------------------------------------
	// Agent main configuration
	// ----------------------------------------
	// Gateway endpoint
	const char *gwyEndpoint = ini.GetValue("agent", "gatewayEndpoint", NULL );
	if (gwyEndpoint != NULL) {
		StringCbCopy(cfg->gatewayEndpoint, INTERNET_MAX_URL_LENGTH, gwyEndpoint);
		syslog(JK_LOG_DEBUG_LEVEL, "'gatewayEndpoint' found in configuration 'agent' section %s", gwyEndpoint);
	} else {
		ok = false;
		syslog(JK_LOG_ERROR_LEVEL, "'gatewayEndpoint' not found in configuration 'agent' section");
	}

	// Gateway login URL
	const char *gwyLoginUrl = ini.GetValue("agent", "gatewayLoginUrl", NULL );
	if (gwyLoginUrl != NULL) {
		StringCbCopy(cfg->gatewayLoginUrl, INTERNET_MAX_URL_LENGTH, gwyLoginUrl);
	} else {
		ok = false;
		syslog(JK_LOG_ERROR_LEVEL, "'gatewayLoginUrl' not found in configuration 'agent' section");
	}

	// SSOSessionManager service path
	const char *sessionManagerServicePath = ini.GetValue("agent", "sessionManagerServicePath", DEFAULT_SESSION_MANAGER_SERVICE_PATH );
	if (sessionManagerServicePath != NULL) {
		StringCbCopy(cfg->sessionManagerServicePath, INTERNET_MAX_URL_LENGTH, sessionManagerServicePath);
		syslog(JK_LOG_DEBUG_LEVEL, "'SSOSessionManager service path: %s", sessionManagerServicePath);
	}

	// SSOIdentityManager service path
	const char *identityManagerServicePath = ini.GetValue("agent", "identityManagerServicePath", DEFAULT_IDENTITY_MANAGER_SERVICE_PATH );
	if (identityManagerServicePath != NULL) {
		StringCbCopy(cfg->identityManagerServicePath, INTERNET_MAX_URL_LENGTH, identityManagerServicePath);
		syslog(JK_LOG_DEBUG_LEVEL, "'SSOIdentityManager service path: %s", identityManagerServicePath);
	}

	// SSOIdentityProvider service path
	const char *identityProviderServicePath = ini.GetValue("agent", "identityProviderServicePath", DEFAULT_IDENTITY_PROVIDER_SERVICE_PATH );
	if (identityProviderServicePath != NULL) {
		StringCbCopy(cfg->identityProviderServicePath, INTERNET_MAX_URL_LENGTH, identityProviderServicePath);
		syslog(JK_LOG_DEBUG_LEVEL, "'SSOIdentityProvider service path: %s", identityProviderServicePath);
	}

	// Session access min interval
	cfg->sessionAccessMinInterval = ini.GetLongValue("agent", "sessionAccessMinInterval", DEFAULT_SESSION_ACCESS_MIN_INTERVAL);
	
	// Cache cleanup min interval
	cfg->cacheCleanupMinInterval = ini.GetLongValue("agent", "cacheCleanupMinInterval", DEFAULT_CACHE_CLEANUP_MIN_INTERVAL);
	
    CSimpleIniA::TNamesDepend sections;
    ini.GetAllSections(sections);
	
    CSimpleIniA::TNamesDepend::const_iterator i;
    for (i = sections.begin(); i != sections.end(); ++i) {
		// syslog(JK_LOG_DEBUG_LEVEL, "section [%s]", i->pItem);

		const char *section = i->pItem;

		// Check partner apps sections
		if (section != NULL &&
			strlen(section) >= 10 &&
			strncmp(section, "partnerapp", 10) == 0) {

			// This is a partner appc

			// Get base-uri
			const char *baseUri = ini.GetValue(section, "base-uri", NULL );
			const char *splashResource = ini.GetValue(section, "splash-resource", NULL);
			const char *partnerAppId = ini.GetValue(section, "partnerAppId", NULL);

			if (baseUri == NULL) {
				syslog(JK_LOG_WARNING_LEVEL, "'base-uri' not found in '%s' section", section);
			} else {

				string s (section);
				string u (baseUri);
				
				// Create partner app configuration
				PartnerAppConfig *appCfg = new PartnerAppConfig(s, u);

				if (splashResource != NULL) {
					string sr (splashResource);
					appCfg->setSplashResource(sr);
				}

				if (partnerAppId != NULL) {
					string appId (partnerAppId);
					appCfg->setPartnerAppId(appId);
				}

				cfg->apps.push_back(*appCfg);
			}			

		} else if (section != NULL &&
			strlen(section) >= 19 &&
			strncmp(section, "security-constraint", 19) == 0) {

			const char *roles = ini.GetValue(section, "roles", NULL );
			if (roles == NULL) {
				syslog(JK_LOG_DEBUG_LEVEL, "'roles' not found in '%s' section", section);
			}

			const char *baseUris = ini.GetValue(section, "base-uris", NULL );
			if (baseUris == NULL) {
				syslog(JK_LOG_WARNING_LEVEL, "'base-uris' not found in '%s' section", section);
			}

			
			if (baseUris != NULL) {

				// Security constratin
				string s (section);
				SecurityConstraintConfig *secCfg = new  SecurityConstraintConfig (s);				

				// List of base uris
				string b (baseUris);
				StringUtil::tokenize(b, secCfg->baseUris, ",");

				// List of roles
				if (roles != NULL) {
					string r (roles);
					StringUtil::tokenize(r, secCfg->roles, ",");
				}

				cfg->secConstraints.push_back(*secCfg);
			}


		}
    }

	return true;

}

bool AbstractSSOAgent::stop() {
	syslog(JK_LOG_INFO_LEVEL, "Stopping JOSSO C Agent [%s]", (VERSION_STRING));
	
	bool ok = true;
	if(!jk_close_file_logger(&logger)) {
		syslog(JK_LOG_ERROR_LEVEL, "Cannot close logger");
		ok = false;
	}

	started = false;
	return ok;
}

// -------------------------------------------------------------------
// Agent Operations
// -------------------------------------------------------------------
bool AbstractSSOAgent::isAuthorized(SSOAgentRequest *req) {
	string path (req->getPath());
	SecurityConstraintConfig * sec = getSecurityConstraintConfig(path);

	if (sec == NULL) {
		jk_log(logger, JK_LOG_DEBUG, "PATH : %s does not have a security constraint, authorizing.", path.c_str());
		return true;
	}

	if (sec->roles.empty()) {
		jk_log(logger, JK_LOG_DEBUG, "PATH : %s has %s security constraint without roles, authorizing.", path.c_str(), sec->id.c_str());
		return true;
	}

	// Check user roles vs sec constraint required roles!
	vector<string>::const_iterator roles;
	for (roles = sec->roles.begin() ; roles < sec->roles.end() ; roles ++) {
		if ( req->isUserInRole(*roles)) {
			jk_log(logger, JK_LOG_DEBUG, "PATH : %s has %s security constraint, role %s found, authorizing", path.c_str(), sec->id.c_str(), (*roles).c_str());
			return true;
		}
	}

	jk_log(logger, JK_LOG_DEBUG, "PATH : %s has %s security constraint, rejecting.", path.c_str(), sec->id.c_str());

	return false;
}

bool AbstractSSOAgent::createSecurityContext(SSOAgentRequest *req) {

	string ssoSession = req->getCookie("JOSSO_SESSIONID");
	string originalResource = req->getCookie("JOSSO_RESOURCE");

	string plainTextOriginalResource = StringUtil::decode64(originalResource);
	
	bool ok = true;

	if (!ssoSession.empty() && ssoSession.compare("-") != 0) {

		jk_log(logger, JK_LOG_DEBUG, "Found SSO Session %s", ssoSession.c_str());

		if (this->accessSession(ssoSession, req)) {

			// Get Principal and Roles
			string principal;
			map<string, string> properties;
			if (ok && !this->findUserInSession(ssoSession, principal, properties, req)) {
				jk_log(logger, JK_LOG_ERROR, "Failed to execute find user in session.");
				ok = false;
			}

			// Roles
			vector<string> roles;
			if (ok && !this->findRolesInSession(ssoSession, roles, req)) {
				jk_log(logger, JK_LOG_ERROR, "Failed to execute find roles in session.");
				ok = false;
			}

			// Debug ...
			if (ok) {
				jk_log(logger, JK_LOG_DEBUG, "SSO Session %s bound with principal %s", ssoSession.c_str(), principal.c_str());

				map<string, string>::iterator ip;
				for (ip = properties.begin() ; ip != properties.end() ; ip ++) {
					jk_log(logger, JK_LOG_DEBUG, "SSO Session %s bound usr property %s:%s", ssoSession.c_str(), (*ip).first.c_str(), (*ip).second.c_str());
				}

				string allRoles;
				vector<string>::const_iterator i;
				for (i = roles.begin() ; i < roles.end() ; i++) {
					allRoles.append((*i));
					allRoles.append(" ");
				}
				jk_log(logger, JK_LOG_DEBUG, "SSO Session %s has %s roles", ssoSession.c_str(), allRoles.c_str());

				// Set security context authorization information.
				req->secCtx.setAuthN(plainTextOriginalResource, ssoSession, principal, properties, roles);

				// Export Security Context
				if (!req->exportSecurityContext(req->secCtx)) {
					jk_log(logger, JK_LOG_ERROR, "Cannot export security context!");
				} else  {
					jk_log(logger, JK_LOG_TRACE, "Exported security context ... OK");
				}
			}

		} else {
			jk_log(logger, JK_LOG_DEBUG, "Cannot access session %s", ssoSession.c_str());
			ok = false;
		}


		// Create SSO Security context

		// req->setAuhtN();
	}

	return ok;
}

bool AbstractSSOAgent::intervalPassed(FILETIME fromTime, long milliseconds) {
	bool intervalPassed = false;

	FILETIME now;
	GetSystemTimeAsFileTime(&now);

	// subtract session access min interval
	ULARGE_INTEGER uLargeInteger;
	uLargeInteger.HighPart = now.dwHighDateTime;
	uLargeInteger.LowPart = now.dwLowDateTime;
	// now we can add (or subtract) the datepart we want
	uLargeInteger.QuadPart -= milliseconds * nano100SecInMilliSec;
	// convert back
	now.dwHighDateTime = uLargeInteger.HighPart;
	now.dwLowDateTime = uLargeInteger.LowPart;
	
	if (CompareFileTime(&now, &fromTime) == 1) {
		intervalPassed = true;
	}

	return intervalPassed;
}

void AbstractSSOAgent::removeUnusedCacheEntries() {
	if (intervalPassed(lastCacheCleanupTime, this->getCacheCleanupMinInterval())) {
		EnterCriticalSection(&cacheMapLock);

		map<string, FILETIME>::iterator cacheIterator;
		for (cacheIterator = cache.begin() ; cacheIterator != cache.end() ;) {
			if (intervalPassed((*cacheIterator).second, this->getSessionAccessMinInterval())) {
				cache.erase(cacheIterator++);
			} else {
				++cacheIterator;
			}
		}

		GetSystemTimeAsFileTime(&lastCacheCleanupTime);

		LeaveCriticalSection(&cacheMapLock);
	}
}

// WS Operations

bool AbstractSSOAgent::accessSession(string ssoSessionId, SSOAgentRequest *ssoAgentReq) {

	bool ok = true;
	bool access = false;

	EnterCriticalSection(&cacheMapLock);
	FILETIME lastAccessTime = cache[ssoSessionId];
	LeaveCriticalSection(&cacheMapLock);

	if (lastAccessTime.dwHighDateTime == 0 || intervalPassed(lastAccessTime, this->getSessionAccessMinInterval())) {
		access = true;
	}
	
	if (access) {
		string endpoint = getGatewaySessionManagerServiceEndpoint();
	
		SSOSessionManagerSOAPBindingProxy svc;
		svc.soap_endpoint = endpoint.c_str();
	
		ns3__AccessSessionRequestType *req = new ns3__AccessSessionRequestType();
		ns3__AccessSessionResponseType res;
	
		req->ssoSessionId.assign(ssoSessionId);
		req->requester.assign(getRequester(ssoAgentReq));
	
		int rc = svc.accessSession(req, &res);
		if ( rc == SOAP_OK) {
			GetSystemTimeAsFileTime(&lastAccessTime);
			EnterCriticalSection(&cacheMapLock);
			cache[ssoSessionId] = lastAccessTime;
			LeaveCriticalSection(&cacheMapLock);
			jk_log(logger, JK_LOG_DEBUG, "Session is still valid %s", ssoSessionId.c_str());
		} else {
			// SSO Session is no longer valid
			EnterCriticalSection(&cacheMapLock);
			cache.erase(ssoSessionId);
			LeaveCriticalSection(&cacheMapLock);
			jk_log(logger, JK_LOG_ERROR, "SOAP Error %d at [%s]", rc, svc.soap_endpoint);
			ok = false;
		}
	
		delete req;
	}

	jk_log(logger, JK_LOG_TRACE, "outcome %d", ok);

	removeUnusedCacheEntries();
	
	return ok;
}



bool AbstractSSOAgent::resolveAssertion(const string assertionId, string & ssoSessionId, SSOAgentRequest *ssoAgentReq) {

	bool ok = true;
	string endpoint = getGatewayIdentityProviderServiceEndpoint();

	SSOIdentityProviderSOAPBindingProxy svc;
	svc.soap_endpoint = endpoint.c_str();

	ns3__ResolveAuthenticationAssertionRequestType *req = new ns3__ResolveAuthenticationAssertionRequestType ();
	ns3__ResolveAuthenticationAssertionResponseType res;

	req->assertionId.assign(assertionId);
	req->requester.assign(getRequester(ssoAgentReq));

	int rc = svc.resolveAuthenticationAssertion(req, &res);
	if ( rc == SOAP_OK) {
		ssoSessionId.assign(res.ssoSessionId);
		jk_log(logger, JK_LOG_DEBUG, "Assertion resolved to %s", ssoSessionId.c_str());
	} else {
		jk_log(logger, JK_LOG_ERROR, "SOAP Error %d at [%s]", rc, svc.soap_endpoint);
		ok = false;
	}

	delete req;

	return ok;
}

bool AbstractSSOAgent::findUserInSession(const string ssoSessionId, string & principal, map<string, string> & properties, SSOAgentRequest *ssoAgentReq) {

	bool ok = true;
	string endpoint = getGatewayIdentityManagerServiceEndpoint();

	SSOIdentityManagerSOAPBindingProxy svc;
	svc.soap_endpoint = endpoint.c_str();

	ns3__FindUserInSessionRequestType *req = new ns3__FindUserInSessionRequestType ();
	ns3__FindUserInSessionResponseType res;

	req->ssoSessionId.assign(ssoSessionId);
	req->requester.assign(getRequester(ssoAgentReq));

	int rc = svc.findUserInSession(req, &res);
	if ( rc == SOAP_OK) {
		principal.assign(res.ns3__SSOUser->name);

		vector<class ns3__SSONameValuePairType * > nv = res.ns3__SSOUser->properties;
		
		vector<class ns3__SSONameValuePairType * >::iterator it;
		for (it = nv.begin() ; it != nv.end() ; it ++) {
			string name = (*it)->name;
			string value = (*it)->value;
			properties [name] = value;
		}

		jk_log(logger, JK_LOG_DEBUG, "User found is %s", principal.c_str());
	} else {
		jk_log(logger, JK_LOG_ERROR, "SOAP Error %d at [%s]", rc, svc.soap_endpoint);
		ok = false;
	}

	delete req;

	return ok;
}

bool AbstractSSOAgent::findRolesInSession(const string ssoSessionId, vector<string> &r, SSOAgentRequest *ssoAgentReq) {

	bool ok = true;
	string endpoint = getGatewayIdentityManagerServiceEndpoint();

	SSOIdentityManagerSOAPBindingProxy svc;
	svc.soap_endpoint = endpoint.c_str();

	ns3__FindRolesBySSOSessionIdRequestType *req = new ns3__FindRolesBySSOSessionIdRequestType();
	ns3__FindRolesBySSOSessionIdResponseType res;

	req->ssoSessionId.assign(ssoSessionId);
	req->requester.assign(getRequester(ssoAgentReq));

	int rc = svc.findRolesBySSOSessionId(req, &res);
	if ( rc == SOAP_OK) {
		// principal.assign(res.FindUserInSessionResponse->ns3__SSOUser->name);
		std::vector<class ns3__SSORoleType * > roles = res.roles;

		vector<class ns3__SSORoleType * >::iterator it;
		for (it = roles.begin() ; it != roles.end() ; it ++) {
			string roleName = (*it)->name;
			r.push_back(roleName);
		}




		// jk_log(logger, JK_LOG_DEBUG, "User found is %s", principal.c_str());
	} else {
		jk_log(logger, JK_LOG_ERROR, "SOAP Error %d at [%s]", rc, svc.soap_endpoint);
		ok = false;
	}

	delete req;

	return ok;
}

/**
 *      1) Required     - The LoginModule is required to succeed.
 *			If it succeeds or fails, authentication still continues
 *			to proceed down the LoginModule list.
 *
 *      2) Sufficient   - The LoginModule is not required to
 *			succeed.  If it does succeed, control immediately
 *			returns to the application (authentication does not
 *			proceed down the LoginModule list).
 *			If it fails, authentication continues down the
 *			LoginModule list.
 *
 *      3) Optional     - The LoginModule is not required to
 *			succeed.  If it succeeds or fails,
 *			authentication still continues to proceed down the
 *			LoginModule list.
 */
bool AbstractSSOAgent::isAutomaticLoginRequired(SSOAgentRequest *req, SSOAgentResponse *res) {
	// If any required module returns false, this will be false
    bool requiredFlag = false;
	bool requiredFlagSet = false;

    // If any sufficient module returns true, this will be true
    bool sufficientFlag = false;
	bool sufficientFlagSet = false;

	list<AbstractAutomaticLoginStrategy*>::iterator as;

	for (as = this->automaticStrategies.begin(); as != this->automaticStrategies.end() ; as++ ) {

		if (!strcmp((*as)->getMode(), JOSSO_AUTH_LOGIN_SUFFICIENT)) {

            if ((*as)->isAutomaticLoginRequired(req, res)) {
                sufficientFlag = true;
				sufficientFlagSet = true;
                break; // Stop evaluation
            }
        }

        if (!strcmp((*as)->getMode(), JOSSO_AUTH_LOGIN_REQUIRED)) {

            if (!(*as)->isAutomaticLoginRequired(req, res)) {
                requiredFlag = false;
				requiredFlagSet = true;
            } else if (!requiredFlagSet) {
			    requiredFlag = true;
				requiredFlagSet = true;
            }
        }

        // This does not affect the outcome of the evaluation
		if (!strcmp((*as)->getMode(), JOSSO_AUTH_LOGIN_OPTIONAL)) {
            (*as)->isAutomaticLoginRequired(req, res);
        }

    }
    // If any required module returned a value, use it.
    if (requiredFlagSet) {
	    return requiredFlag;
    }

    // If any sufficient modules returned a value, use it; otherwise return false.
    return sufficientFlagSet && sufficientFlag;
}

// -------------------------------------------------------------------
// Utils 
// -------------------------------------------------------------------


AgentConfig *AbstractSSOAgent::getAgentConfig() {
	return this->agentConfig;
}

PartnerAppConfig *AbstractSSOAgent::getPartnerAppConfig(const string & path) {

	PartnerAppConfig *cfg = NULL;
	list<PartnerAppConfig>::iterator app;

	string p (path);
	std::transform(p.begin(), p.end(), p.begin(), tolower);

	size_t maxLength = 0;
	for (app = this->agentConfig->apps.begin() ; app != this->agentConfig->apps.end() ; app ++ ) {
		string baseUri = app->baseUri;
		std::transform(baseUri.begin(), baseUri.end(), baseUri.begin(), tolower);
		
		size_t pos = p.find(baseUri);
		if (pos != string::npos && pos==0) {
			// Now, we match the longest baseUri
			if (app->baseUri.length() > maxLength) {
				maxLength = app->baseUri.length();
				cfg = &(*app);
			}
		}
	}
	return cfg;
}

SecurityConstraintConfig *AbstractSSOAgent::getSecurityConstraintConfig(const string & path) {

	SecurityConstraintConfig *cfg = NULL;
	list<SecurityConstraintConfig>::iterator sec;

	string p (path);
	std::transform(p.begin(), p.end(), p.begin(), tolower);

	size_t maxLength = 0;
	for (sec = this->agentConfig->secConstraints.begin() ; sec != this->agentConfig->secConstraints.end() ; sec ++ ) {

		SecurityConstraintConfig s = *sec;
		vector<string>::iterator baseUri;

		for (baseUri = s.baseUris.begin() ; baseUri != s.baseUris.end() ; baseUri ++) {
			string b = *baseUri;
			std::transform(b.begin(), b.end(), b.begin(), tolower);
			size_t pos = p.find(b);

			jk_log(logger, JK_LOG_DEBUG, "Matching path %s against security constraint URI %s", path.c_str(), b.c_str());


			if ( match(p, b) == true ) {
					cfg = &(*sec);
					jk_log(logger, JK_LOG_DEBUG, "Matched path %s against security constraint URI %s", path.c_str(), b.c_str());
			} 


			/*
			if (pos != string::npos && pos==0) {
				if (b.length() > maxLength) {
					maxLength = b.length();
					cfg = &(*sec);
				}
			}
			*/
		}	

	}
	return cfg;
}

string AbstractSSOAgent::getGatewayIdentityManagerServiceEndpoint() {
	// TODO : Support secure channel
	// TODO : Support configuring... hardcoding the use of ssl
	string endpoint ("https://");
	endpoint.append(agentConfig->getGatewayEndpoint());
	endpoint.append("/");
	endpoint.append(agentConfig->getIdentityManagerServicePath());

	return endpoint;
}

string AbstractSSOAgent::getGatewayIdentityProviderServiceEndpoint() {
	// TODO : Support secure channel
	// TODO : Support configuring... hardcoding the use of ssl
	string endpoint ("https://");
	endpoint.append(agentConfig->getGatewayEndpoint());
	endpoint.append("/");
	endpoint.append(agentConfig->getIdentityProviderServicePath());

	return endpoint;
}


string AbstractSSOAgent::getGatewaySessionManagerServiceEndpoint() {
	// TODO : Support secure channel
	// TODO : Support configuring... hardcoding the use of ssl
	string endpoint ("https://");
	endpoint.append(agentConfig->getGatewayEndpoint());
	endpoint.append("/");
	endpoint.append(agentConfig->getSessionManagerServicePath());

	return endpoint;
}


void AbstractSSOAgent::logSoapFault(struct soap *soap) { 
	if (soap_check_state(soap))
		jk_log(logger, JK_LOG_ERROR, "Error: soap struct state not initialized\n");

	else if (soap->error) { 
		const char *c, *v = NULL, *s, **d;
		d = soap_faultcode(soap);
		if (!*d)
			soap_set_fault(soap);
		c = *d;
		if (soap->version == 2)
			v = *soap_faultsubcode(soap);
		s = *soap_faultstring(soap);
		d = soap_faultdetail(soap);

		jk_log(logger, JK_LOG_ERROR,  "%s%d fault: %s [%s]\n\"%s\"\nDetail: %s\n", soap->version ? "SOAP 1." : "Error ", soap->version ? (int)soap->version : soap->error, c, v ? v : "no subcode", s ? s : "[no reason]", d && *d ? *d : "[no detail]");
  }
}

bool AbstractSSOAgent::match(const string &source, const string &regex_string) {

  struct re_pattern_buffer regex;
  struct re_registers regs;
  int n;
  int match;
  bool result = false;

  memset (&regex, '\0', sizeof (regex));

  n = regcomp (&regex, regex_string.c_str(), REG_EXTENDED);

  if (n != 0)
    {
		jk_log(logger, JK_LOG_ERROR, "re_compile_pattern return non-NULL value : source is %s and regex is regex_string %s", source.c_str(), regex_string.c_str());
    }
  else
    {
		match = re_match (&regex, source.c_str(), source.length(), 0, &regs);
		jk_log(logger, JK_LOG_DEBUG, "match returned %d", match);
      if (match > 0)
	{
	  result = true;
	}

	regfree(&regex);
  }

  return result;
  
}

// ---------------------------------------------------------------------------------------------
