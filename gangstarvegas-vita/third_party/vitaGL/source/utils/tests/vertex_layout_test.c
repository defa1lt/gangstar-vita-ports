#include "../vertex_layout.h"
#include <assert.h>
#include <stdint.h>

int main(void) {
    /* Color/position pass the old first-pair check; UV precedes their base. */
    assert(vgl_client_attribute_is_packed(0x1008, 0x100c, 24, 24));
    assert(!vgl_client_attribute_is_packed(0x1008, 0x1000, 24, 24));
    assert(vgl_client_attribute_is_packed(0x1000, 0x1014, 24, 24));
    assert(!vgl_client_attribute_is_packed(0x1000, 0x1018, 24, 24));
    assert(!vgl_client_attribute_is_packed(0x1000, 0x1ee4, 24, 24));
    assert(!vgl_client_attribute_is_packed(0x1000, 0x100c, 24, 12));
    assert(!vgl_client_attribute_is_packed(0x1000, 0x1000, 0, 0));
    assert(vgl_client_attribute_is_packed(UINT32_MAX - 15, UINT32_MAX - 1, 24, 24));
    assert(!vgl_client_attribute_is_packed(UINT32_MAX - 15, 4, 24, 24));
    return 0;
}
