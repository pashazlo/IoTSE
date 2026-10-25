#include "tx_pmkid_select.h"

#include <string.h>

/* Ранг предпочтения AKM: больше — желаннее. Для PMKID-атаки типовая цель —
 * WPA2-PSK, поэтому PSK выше всех; SHA256-вариант PSK следом; SAE (WPA3) ниже —
 * его берём, только если PSK у точки нет. Всё остальное — крайний случай. */
static int akm_rank(wifi_akm_kind_t kind)
{
    switch (kind) {
    case WIFI_AKM_PSK:        return 3;
    case WIFI_AKM_PSK_SHA256: return 2;
    case WIFI_AKM_SAE:        return 1;
    default:                  return 0;
    }
}

bool tx_pmkid_rsn_selection_from_info(const wifi_rsn_info_t *info,
                                      tx_rsn_selection_t *out)
{
    if (info == NULL || out == NULL) {
        return false;
    }
    /* Без хотя бы одного pairwise и одного AKM assoc-req собрать нечем — пусть
     * вызывающий подставит дефолт (передаст NULL в сборщик). */
    if (info->pairwise_count == 0U || info->akm_count == 0U) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    /* Версия: как у точки; 0 быть не должно, но на всякий случай нормализуем к 1. */
    out->version = (info->version != 0U) ? info->version : 1U;

    /* Group cipher — как есть (станция не выбирает group, он один у сети). */
    memcpy(out->group_cipher, info->group_cipher.raw, 4);

    /* Pairwise: предпочитаем CCMP-128, иначе первый из списка точки. Границу по
     * pairwise_count дополнительно ограничиваем ёмкостью массива. */
    uint8_t p_count = info->pairwise_count;
    if (p_count > WIFI_RSN_MAX_PAIRWISE_CIPHERS) {
        p_count = WIFI_RSN_MAX_PAIRWISE_CIPHERS;
    }
    uint8_t pi = 0U;
    for (uint8_t i = 0U; i < p_count; ++i) {
        if (info->pairwise[i].kind == WIFI_CIPHER_CCMP_128) {
            pi = i;
            break;
        }
    }
    memcpy(out->pairwise_cipher, info->pairwise[pi].raw, 4);

    /* AKM: выбираем с максимальным рангом (PSK > PSK-SHA256 > SAE > прочее). */
    uint8_t a_count = info->akm_count;
    if (a_count > WIFI_RSN_MAX_AKMS) {
        a_count = WIFI_RSN_MAX_AKMS;
    }
    uint8_t ai = 0U;
    int best = -1;
    for (uint8_t i = 0U; i < a_count; ++i) {
        int rank = akm_rank(info->akms[i].kind);
        if (rank > best) {
            best = rank;
            ai = i;
        }
    }
    memcpy(out->akm, info->akms[ai].raw, 4);

    /* RSN capabilities — как у точки (там живут PMF-биты и пр.). Если точка их не
     * заявляла, оставляем 0 (memset выше уже обнулил). */
    if (info->has_capabilities) {
        out->rsn_capabilities = info->capabilities;
    }

    return true;
}
