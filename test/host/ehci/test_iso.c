// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include "tusb_option.h"
#include "common/tusb_common.h"

// Only the QH software tail contains a native pointer. On a 64-bit test host
// its size differs from the 32-bit target ABI. Recheck hardware layouts below.
#undef TU_VERIFY_STATIC
#define TU_VERIFY_STATIC(condition, ...) \
  _Static_assert((condition) || sizeof(void*) == 8, "EHCI ABI")
#include "portable/ehci/ehci.h"
#undef TU_VERIFY_STATIC
#define TU_VERIFY_STATIC(condition, ...) _Static_assert(condition, __VA_ARGS__)
#include "portable/ehci/ehci.c"

_Static_assert(sizeof(ehci_link_t) == 4, "link ABI");
_Static_assert(sizeof(ehci_qtd_t) == 32, "qTD ABI");
_Static_assert(offsetof(ehci_qhd_t, qtd_overlay) == 16, "QH hardware prefix");
_Static_assert(sizeof(ehci_itd_t) == 64, "iTD ABI");
_Static_assert(sizeof(ehci_sitd_t) == 32, "siTD ABI");
_Static_assert(sizeof(ehci_cap_registers_t) == 16, "capability register ABI");

static ehci_registers_t regs;
static ehci_cap_registers_t caps;
static tuh_bus_info_t buses[8];
static hcd_event_t event;
static unsigned events;
static unsigned queued_events;
static hcd_event_t terminal_events[16];
static uint8_t buffer[8192] TU_ATTR_ALIGNED(4096);

void hcd_int_enable(uint8_t rhport) { (void) rhport; }
void hcd_int_disable(uint8_t rhport) { (void) rhport; }
void usbh_spin_lock(bool in_isr) { (void) in_isr; }
void usbh_spin_unlock(bool in_isr) { (void) in_isr; }
bool tuh_bus_info_get(uint8_t daddr, tuh_bus_info_t* bus) {
  memset(bus, 0, sizeof(*bus));
  if (daddr >= TU_ARRAY_SIZE(buses)) {
    return false;
  }
  *bus = buses[daddr];
  return true;
}
void hcd_event_handler(hcd_event_t const* e, bool in_isr) {
  (void) in_isr;
  if (e->xfer_complete.result == XFER_RESULT_QUEUED) {
    queued_events++;
    return;
  }
  event = *e;
  assert(events < TU_ARRAY_SIZE(terminal_events));
  terminal_events[events++] = *e;
}

static void reset(uint8_t root_speed) {
  memset(&ehci_data, 0, sizeof(ehci_data));
  memset((void*)&regs, 0, sizeof(regs));
  memset((void*)&caps, 0, sizeof(caps));
  memset(buses, 0, sizeof(buses));
  ehci_data.regs = &regs;
  ehci_data.cap_regs = &caps;
  regs.portsc = (uint32_t)root_speed << 26;
  regs.frame_index = 800;
  regs.command_bm.int_threshold = 8;
  init_periodic_list(0);
  events = queued_events = 0;
}

#if CFG_TUH_EHCI_ISO_EP_MAX
static bool open_ep(uint8_t addr, uint8_t speed, uint16_t size, uint8_t interval) {
  buses[1].speed = speed;
  tusb_desc_endpoint_t desc = {
    .bLength = sizeof(desc), .bDescriptorType = TUSB_DESC_ENDPOINT,
    .bEndpointAddress = addr, .bmAttributes = {.xfer = TUSB_XFER_ISOCHRONOUS},
    .wMaxPacketSize = size, .bInterval = interval
  };
  return iso_ep_open(0, 1, &desc);
}

static void test_native_fs(void) {
  reset(TUSB_SPEED_FULL);
  assert(open_ep(0x81, TUSB_SPEED_FULL, 1023, 1));
  iso_ep_t* ep = iso_ep_find(1, 0x81);
  assert(iso_xfer(0, ep, buffer + 4090, 1023));
  ehci_sitd_t* td = &iso_td(ep, &ep->req[ep->head])->sitd;
  assert(ep->req[ep->head].scheduled_uframe == 800);
  assert(td->active && td->int_on_complete && td->total_bytes == 1023);
  assert(td->int_smask == 0 && td->fl_int_cmask == 0);
  assert(td->buffer[0] == (uint32_t)(uintptr_t)(buffer + 4090));
  assert(td->buffer[1] == (uint32_t)(uintptr_t)(buffer + 4096));

  td->active = 0;
  td->total_bytes = 23;
  regs.frame_index = 808;
  iso_process(true);
  assert(events == 1 && event.xfer_complete.len == 1000);
  assert(event.xfer_complete.result == XFER_RESULT_SUCCESS);
  iso_process(true);
  assert(events == 1);
  assert(iso_xfer(0, ep, buffer, 0));
  td = &iso_td(ep, &ep->req[ep->head])->sitd;
  td->active = 0;
  regs.frame_index += 8;
  iso_process(true);
  assert(events == 2 && event.xfer_complete.len == 0);
  assert(event.xfer_complete.result == XFER_RESULT_SUCCESS);
}

static void test_split(void) {
  reset(TUSB_SPEED_HIGH);
  buses[1].hub_addr = 2;
  buses[1].hub_port = 3;
  buses[2].speed = TUSB_SPEED_FULL;
  buses[2].hub_addr = 3;
  buses[2].hub_port = 4;
  buses[3].speed = TUSB_SPEED_HIGH;
  assert(!open_ep(0x81, TUSB_SPEED_FULL, 565, 1));
  assert(open_ep(0x81, TUSB_SPEED_FULL, 564, 1));
  iso_ep_t* ep = iso_ep_find(1, 0x81);
  assert(iso_xfer(0, ep, buffer, 564));
  ehci_sitd_t* td = &iso_td(ep, &ep->req[ep->head])->sitd;
  assert(td->hub_addr == 3 && td->port_number == 4);
  assert(td->int_smask == 4 && td->fl_int_cmask == 0xf0);
  assert(open_ep(1, TUSB_SPEED_FULL, 1023, 1));
  ep = iso_ep_find(1, 1);
  assert(iso_xfer(0, ep, buffer, 1023));
  td = &iso_td(ep, &ep->req[ep->head])->sitd;
  assert(td->int_smask == 0x3f && td->fl_int_cmask == 0);
  assert((td->buffer[1] & 0xfff) == (6 | 8));
}

static void test_split_audio(void) {
  reset(TUSB_SPEED_HIGH);
  buses[1].hub_addr = 2;
  buses[1].hub_port = 1;
  buses[2].speed = TUSB_SPEED_HIGH;
  assert(open_ep(0x81, TUSB_SPEED_FULL, 98, 1));
  assert(open_ep(0x01, TUSB_SPEED_FULL, 196, 1));
  iso_ep_t* in = iso_ep_find(1, 0x81);
  iso_ep_t* out = iso_ep_find(1, 0x01);
  assert(iso_xfer(0, in, buffer, 98));
  assert(iso_xfer(0, out, buffer + 128, 192));
  ehci_sitd_t* in_td = &iso_td(in, &in->req[in->head])->sitd;
  ehci_sitd_t* out_td = &iso_td(out, &out->req[out->head])->sitd;
  // 48 kHz stereo needs two start-splits. Do not interleave an IN start
  // with the OUT Begin/End sequence on the same transaction translator.
  assert(out_td->int_smask == 3);
  assert((out_td->buffer[1] & 0x1f) == (2 | 8));
  assert((in_td->int_smask & out_td->int_smask) == 0);
  assert(in_td->int_smask > out_td->int_smask);
}

static void test_hs(void) {
  reset(TUSB_SPEED_HIGH);
  assert(open_ep(0x82, TUSB_SPEED_HIGH, 1024 | (2 << 11), 1));
  iso_ep_t* ep = iso_ep_find(1, 0x82);
  assert(!iso_xfer(0, ep, buffer, 3073));
  assert(iso_xfer(0, ep, buffer + 4095, 3072));
  ehci_itd_t* td = &iso_td(ep, &ep->req[ep->head])->itd;
  uint8_t slot = ep->req[ep->head].scheduled_uframe & 7;
  assert(ep->req[ep->head].scheduled_uframe == 802);
  assert(((uintptr_t)td & 63) == 0);
  assert(td->xact[slot].offset == 4095 && td->xact[slot].length == 3072);
  assert((td->BufferPointer[0] & 0xfff) == 0x201);
  assert((td->BufferPointer[1] & 0xfff) == 0xc00);
  assert((td->BufferPointer[2] & 0xfff) == 3);
  assert((td->BufferPointer[0] & ~0xfffu) == (uint32_t)(uintptr_t)buffer);
  assert((td->BufferPointer[1] & ~0xfffu) == (uint32_t)(uintptr_t)(buffer + 4096));
  assert((td->BufferPointer[2] & ~0xfffu) == (uint32_t)(uintptr_t)(buffer + 8192));
  for (unsigned i = 0; i < 8; i++) {
    assert(td->xact[i].active == (i == slot));
  }
  td->xact[slot].active = 0;
  td->xact[slot].length = 2048;
  regs.frame_index = 803;
  iso_process(true);
  assert(events == 1 && event.xfer_complete.len == 2048);
  assert(iso_xfer(0, ep, buffer, 10));
  td = &iso_td(ep, &ep->req[ep->head])->itd;
  slot = ep->req[ep->head].scheduled_uframe & 7;
  td->xact[slot].active = 0;
  td->xact[slot].babble_err = 1;
  regs.frame_index = ep->req[ep->head].scheduled_uframe + 1;
  iso_process(true);
  assert(events == 2 && event.xfer_complete.result == XFER_RESULT_FAILED);
  assert(event.xfer_complete.len == 0);
}

static void test_long_interval_and_wrap(void) {
  reset(TUSB_SPEED_HIGH);
  assert(open_ep(1, TUSB_SPEED_HIGH, 64, 10));
  iso_ep_t* ep = iso_ep_find(1, 1);
  assert(iso_xfer(0, ep, buffer, 64));
  assert(!ep->req[ep->head].armed && ep->req[ep->head].scheduled_uframe == 1312);
  regs.frame_index = 1264;
  iso_process(true);
  assert(ep->req[ep->head].armed && events == 0);
  iso_td(ep, &ep->req[ep->head])->itd.xact[0].active = 0;
  regs.frame_index = 1313;
  iso_process(true);
  assert(events == 1 && event.xfer_complete.len == 64);
  assert(iso_xfer(0, ep, buffer, 64));
  assert(!ep->req[ep->head].armed && iso_abort(0, ep));
  assert(ep->count == 0 && events == 1);
  // Start a separate long interval request and let its arm window expire.
  ep->next_uframe = regs.frame_index + ep->interval;
  assert(iso_xfer(0, ep, buffer, 64));
  regs.frame_index = ep->req[ep->head].scheduled_uframe + 1;
  iso_process(true);
  assert(ep->count == 0 && events == 2 && event.xfer_complete.result == XFER_RESULT_FAILED);
  ehci_data.iso_last_frindex = 16380;
  ehci_data.iso_uframe = 0xfffffffcu;
  regs.frame_index = 4;
  assert(iso_now() == 4);
}

#endif

static void test_qtd_retirement(void) {
  reset(TUSB_SPEED_FULL);
  ehci_qhd_t* qh = &ehci_data.control[1].qhd;
  ehci_qtd_t* td = &ehci_data.control[1].qtd;
  qh->dev_addr = 1;
  qh->attached_qtd = td;
  td->active = 1;
  td->expected_bytes = 3;
  qhd_xfer_complete_isr(qh);
  assert(events == 0 && qh->attached_qtd == td);
  td->active = 0;
  qhd_xfer_complete_isr(qh);
  assert(events == 1 && event.xfer_complete.len == 3);
  qhd_xfer_complete_isr(qh);
  assert(events == 1);
}

#if CFG_TUH_EHCI_ISO_EP_MAX
static void test_limits_and_late_completion(void) {
  reset(TUSB_SPEED_HIGH);
  assert(!open_ep(0x80, TUSB_SPEED_HIGH, 64, 1));
  assert(!open_ep(0x81, TUSB_SPEED_LOW, 64, 1));
  assert(!open_ep(0x81, TUSB_SPEED_HIGH, 64, 0));
  assert(!open_ep(0x81, TUSB_SPEED_HIGH, 64, 17));
  assert(!open_ep(0x81, TUSB_SPEED_HIGH, 0, 1));
  assert(!open_ep(0x81, TUSB_SPEED_HIGH, 1025, 1));
  assert(!open_ep(0x81, TUSB_SPEED_HIGH, 64 | (3 << 11), 1));
  for (unsigned i = 1; i <= CFG_TUH_EHCI_ISO_EP_MAX; i++) {
    assert(open_ep((uint8_t)i, TUSB_SPEED_HIGH, 64, 1));
  }
  assert(!open_ep(CFG_TUH_EHCI_ISO_EP_MAX + 1, TUSB_SPEED_HIGH, 64, 1));
  iso_ep_t* ep = iso_ep_find(1, 1);
  assert(iso_xfer(0, ep, buffer, 64));
  iso_td(ep, &ep->req[ep->head])->itd.xact[ep->req[ep->head].scheduled_uframe & 7].active = 0;
  regs.frame_index = (ep->req[ep->head].scheduled_uframe & ~7u) + FRAMELIST_SIZE * 8u;
  iso_process(true);
  assert(events == 1 && event.xfer_complete.result == XFER_RESULT_FAILED);
  assert(ep->count == 0);
}

#if CFG_TUH_XFER_QUEUE_DEPTH > 1
static void test_queue(void) {
  reset(TUSB_SPEED_HIGH);
  assert(open_ep(0x81, TUSB_SPEED_HIGH, 64, 1));
  iso_ep_t* ep = iso_ep_find(1, 0x81);
  for (unsigned round = 0; round < 3; round++) {
    assert(iso_xfer(0, ep, buffer, 24));
    assert(queued_events == round + 1);
    iso_req_t* first = &ep->req[ep->head];
    assert(iso_xfer(0, ep, buffer + 64, 28));
    iso_req_t* second = &ep->req[(ep->head + 1) % CFG_TUH_XFER_QUEUE_DEPTH];
    assert(queued_events == round + 1 && ep->count == 2);
    assert(!iso_xfer(0, ep, buffer + 128, 32));
    assert(second->scheduled_uframe == first->scheduled_uframe + 1);
    ehci_itd_t* td1 = &iso_td(ep, first)->itd;
    ehci_itd_t* td2 = &iso_td(ep, second)->itd;
    assert(td1 != td2 && first->buffer != second->buffer);
    // A tail completion must never bypass the FIFO head.
    td2->xact[second->scheduled_uframe & 7].active = 0;
    iso_process(true);
    assert(events == round * 2 && ep->count == 2);
    td1->xact[first->scheduled_uframe & 7].active = 0;
    regs.frame_index = second->scheduled_uframe;
    iso_process(true);
    assert(events == (round + 1) * 2 && ep->count == 0);
    assert(terminal_events[round * 2].xfer_complete.len == 24);
    assert(terminal_events[round * 2 + 1].xfer_complete.len == 28);
  }
}

#endif

static void test_schedule_sweep(void) {
  // Exercise every descriptor slot, all intervals and frame-counter wrap.
  for (uint8_t interval = 1; interval <= 16; interval++) {
    for (uint32_t start = 16368; start < 16400; start++) {
      reset(TUSB_SPEED_HIGH);
      regs.frame_index = start & 0x3fff;
      assert(open_ep(0x81, TUSB_SPEED_HIGH, 64, interval));
      iso_ep_t* ep = iso_ep_find(1, 0x81);
      assert(iso_xfer(0, ep, buffer, 64));
      uint32_t const due = ep->req[ep->head].scheduled_uframe;
      uint32_t const now = iso_now();
      assert((int32_t)(due - now) >= 2);
      assert((due - now) % ep->interval == 0);
      assert(ep->req[ep->head].armed == (due - now < (FRAMELIST_SIZE - 1) * 8));
      if (!ep->req[ep->head].armed) {
        assert(iso_abort(0, ep));
      } else {
        iso_td(ep, &ep->req[ep->head])->itd.xact[due & 7].active = 0;
        regs.frame_index = (due + 1) & 0x3fff;
        iso_process(true);
        assert(events == 1 && event.xfer_complete.result == XFER_RESULT_SUCCESS);
      }
    }
  }
}

#endif

int main(void) {
  // Hardware links are 32-bit. The runner places static fixtures below 4 GiB.
  assert((uintptr_t)&ehci_data <= UINT32_MAX && (uintptr_t)buffer <= UINT32_MAX);
  test_qtd_retirement();
#if CFG_TUH_EHCI_ISO_EP_MAX
  test_native_fs();
  test_split();
  test_split_audio();
  test_hs();
  test_long_interval_and_wrap();
  test_limits_and_late_completion();
  test_schedule_sweep();
#if CFG_TUH_XFER_QUEUE_DEPTH > 1
  test_queue();
#endif
#endif
  puts("EHCI ISO regression tests passed");
  return 0;
}
