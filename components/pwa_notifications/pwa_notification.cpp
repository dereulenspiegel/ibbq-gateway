#include "pwa_notification.h"

#define MBEDTLS_DEBUG_C

#include "string.h"
#include "esp_log.h"
#include "esp_err.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "mbedtls/platform.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/pk.h"
#include "mbedtls/debug.h"

#define PWA_NOTIFICATION_STORAGE_NAMESPACE "pwa"

static const char *PWA_STORAGE_KEY_LENGTH = "key_length";
static const char *PWA_STORAGE_KEY = "private_key";

static const char *TAG = "pwa-mbedtls";

const char pers[] = "ibbq";

esp_err_t generate_private_key(uint8_t *buf, size_t *buf_len)
{
    int ret;
    mbedtls_pk_context key;
    //mbedtls_mpi N, P, Q, D, E, DP, DQ, QP;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    /*mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&P);
    mbedtls_mpi_init(&Q);
    mbedtls_mpi_init(&D);
    mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&DP);
    mbedtls_mpi_init(&DQ);
    mbedtls_mpi_init(&QP);*/

    mbedtls_pk_init(&key);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    memset(buf, 0, *buf_len);

    mbedtls_entropy_init(&entropy);
    // type MBEDTLS_PK_ECKEY
    // TODO configure entropy as optimal as possible
    if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     // TODO seed with device specific info like MAC, serial etc.
                                     (const unsigned char *)pers,
                                     sizeof(pers))) != 0)
    {
        ESP_LOGE(TAG, "Failed! mbedtls_ctr_drbg_seed returned %d\n", ret);
        return ESP_FAIL;
    }

    if ((ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type((mbedtls_pk_type_t)MBEDTLS_PK_ECKEY))) != 0)
    {
        ESP_LOGE(TAG, "failed\n  !  mbedtls_pk_setup returned -0x%04x", -ret);
        return ESP_FAIL;
    }

    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256K1,
                              mbedtls_pk_ec(key),
                              mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0)
    {
        ESP_LOGE(TAG, " failed\n  !  mbedtls_ecp_gen_key returned -0x%04x", -ret);
        return ESP_FAIL;
    }

    // Does not seem to work correctly on ESP32, but I am sure we have a valid private key
    if (mbedtls_pk_get_type(&key) == MBEDTLS_PK_ECKEY)
    {
        mbedtls_ecp_keypair *ecp = mbedtls_pk_ec(key);
        if (ecp != NULL)
        {
            ESP_LOGI(TAG, "ECP group id: %d", ecp->grp.id);
            const mbedtls_ecp_curve_info *curve_info = mbedtls_ecp_curve_info_from_grp_id((mbedtls_ecp_group_id)ecp->grp.id);
            if (curve_info != NULL)
            {
                ESP_LOGI(TAG, "Got key infos curve: %s", curve_info->name);
            }
            else
            {
                ESP_LOGE(TAG, "Curve Info was NULL");
            }
        }
        else
        {
            ESP_LOGE(TAG, "ECP was NULL!!!");
            return ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGE(TAG, "Unknown key type");
        return ESP_FAIL;
    }

    if ((ret = mbedtls_pk_write_key_der(&key, (unsigned char *)buf, *buf_len)) < 0)
    {
        char err_buf[256];
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "Failed to serialize private key to DER format: %d: %s", ret, err_buf);
        return ESP_FAIL;
    }
    *buf_len = ret;
    return ESP_OK;
}

esp_err_t store_key(uint8_t *buf, size_t len)
{
    esp_err_t ret;
    nvs_handle my_handle;
    if ((ret = nvs_open(PWA_NOTIFICATION_STORAGE_NAMESPACE, NVS_READWRITE, &my_handle)) != ESP_OK)
    {
        return ret;
    }
    if ((ret = nvs_set_u32(my_handle, PWA_STORAGE_KEY_LENGTH, len)) != ESP_OK)
    {
        nvs_close(my_handle);
        return ret;
    }
    if ((ret = nvs_set_blob(my_handle, PWA_STORAGE_KEY, buf, len)) != ESP_OK)
    {
        nvs_close(my_handle);
        return ret;
    }
    if ((ret = nvs_commit(my_handle)) != ESP_OK)
    {
        nvs_close(my_handle);
        return ret;
    }
    nvs_close(my_handle);

    return ESP_OK;
}

esp_err_t load_key(uint8_t *buf, size_t *len)
{
    esp_err_t ret;
    nvs_handle my_handle;
    uint32_t key_len;
    if ((ret = nvs_open(PWA_NOTIFICATION_STORAGE_NAMESPACE, NVS_READONLY, &my_handle)) != ESP_OK)
    {
        return ret;
    }
    if ((ret = nvs_get_u32(my_handle, PWA_STORAGE_KEY_LENGTH, &key_len)) != ESP_OK)
    {
        nvs_close(my_handle);
        return ret;
    }
    if ((ret = nvs_get_blob(my_handle, PWA_STORAGE_KEY, buf, &key_len)) != ESP_OK)
    {
        nvs_close(my_handle);
        *len = 0;
        return ret;
    }
    *len = key_len;
    nvs_close(my_handle);
    return ESP_OK;
}

esp_err_t pwa_init_keys()
{
    //mbedtls_debug_set_threshold(5);
    esp_err_t ret;
    uint8_t buf[1024];
    size_t len = sizeof(buf);
    ESP_LOGI(TAG, "Loading private key");
    if (load_key(buf, &len) != ESP_OK || len == 0)
    {
        ESP_LOGI(TAG, "Private key not found, generating new one");
        if ((ret = generate_private_key(buf, &len)) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed generating private key");
            return ret;
        }
        uint8_t *key_buf = buf + sizeof(buf) - len;
        if ((ret = store_key(key_buf, len)) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed storing private key");
            return ret;
        }
    }
    return ESP_OK;
}