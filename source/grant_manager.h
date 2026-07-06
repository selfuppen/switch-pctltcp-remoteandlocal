#ifndef GRANT_MANAGER_H
#define GRANT_MANAGER_H

#include <switch.h>
#include <stdbool.h>

#define GRANT_DIR          "sdmc:/switch/pctltcp-sysmodule"
#define GRANT_CONFIG_PATH  GRANT_DIR "/grant.conf"
#define GRANT_REQUEST_PATH GRANT_DIR "/grant_request.json"
#define GRANT_RESULT_PATH  GRANT_DIR "/grant_result.json"
#define GRANT_USED_PATH    GRANT_DIR "/used_grants.dat"

void grant_manager_init(void);
void grant_manager_process(void);

#endif /* GRANT_MANAGER_H */
