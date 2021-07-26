/*
 * GPIO line test cases for libgpiod.
 *
 * Copyright (C) 2017 Bartosz Golaszewski <bartekgola@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2.1 of the GNU Lesser General Public License
 * as published by the Free Software Foundation.
 */

#include "gpiod-test.h"

static void line_request_output(void)
{
	TEST_CLEANUP(test_close_chip) struct gpiod_chip *chip = NULL;
	struct gpiod_line *line_0;
	struct gpiod_line *line_1;
	int status;

	chip = gpiod_chip_open(test_chip_path(0));
	TEST_ASSERT_NOT_NULL(chip);

	line_0 = gpiod_chip_get_line(chip, 2);
	line_1 = gpiod_chip_get_line(chip, 5);
	TEST_ASSERT_NOT_NULL(line_0);
	TEST_ASSERT_NOT_NULL(line_1);

	status = gpiod_line_request_output(line_0, TEST_CONSUMER, false, 0);
	TEST_ASSERT_RET_OK(status);
	status = gpiod_line_request_output(line_1, TEST_CONSUMER, false, 1);
	TEST_ASSERT_RET_OK(status);

	TEST_ASSERT_EQ(gpiod_line_get_value(line_0), 0);
	TEST_ASSERT_EQ(gpiod_line_get_value(line_1), 1);

	gpiod_line_release(line_0);
	gpiod_line_release(line_1);
}
TEST_DEFINE(line_request_output,
	    "gpiod_line_request_output() - good",
	    0, { 8 });

static void line_request_already_requested(void)
{
	TEST_CLEANUP(test_close_chip) struct gpiod_chip *chip = NULL;
	struct gpiod_line *line;
	int status;

	chip = gpiod_chip_open(test_chip_path(0));
	TEST_ASSERT_NOT_NULL(chip);

	line = gpiod_chip_get_line(chip, 0);
	TEST_ASSERT_NOT_NULL(line);

	status = gpiod_line_request_input(line, TEST_CONSUMER, false);
	TEST_ASSERT_RET_OK(status);

	status = gpiod_line_request_input(line, TEST_CONSUMER, false);
	TEST_ASSERT_NOTEQ(status, 0);
	TEST_ASSERT_EQ(gpiod_errno(), GPIOD_ELINEBUSY);
}
TEST_DEFINE(line_request_already_requested,
	    "gpiod_line_request() - already requested",
	    0, { 8 });

static void line_consumer(void)
{
	TEST_CLEANUP(test_close_chip) struct gpiod_chip *chip = NULL;
	struct gpiod_line *line;
	int status;

	chip = gpiod_chip_open(test_chip_path(0));
	TEST_ASSERT_NOT_NULL(chip);

	line = gpiod_chip_get_line(chip, 0);
	TEST_ASSERT_NOT_NULL(line);

	TEST_ASSERT_NULL(gpiod_line_consumer(line));

	status = gpiod_line_request_input(line, TEST_CONSUMER, false);
	TEST_ASSERT_RET_OK(status);

	TEST_ASSERT(!gpiod_line_needs_update(line));
	TEST_ASSERT_STR_EQ(gpiod_line_consumer(line), TEST_CONSUMER);
}
TEST_DEFINE(line_consumer,
	    "gpiod_line_consumer() - good",
	    0, { 8 });

static void line_request_bulk_output(void)
{
	TEST_CLEANUP(test_close_chip) struct gpiod_chip *chipA = NULL;
	TEST_CLEANUP(test_close_chip) struct gpiod_chip *chipB = NULL;
	struct gpiod_line *lineA0;
	struct gpiod_line *lineA1;
	struct gpiod_line *lineA2;
	struct gpiod_line *lineA3;
	struct gpiod_line *lineB0;
	struct gpiod_line *lineB1;
	struct gpiod_line *lineB2;
	struct gpiod_line *lineB3;
	struct gpiod_line_bulk bulkA;
	struct gpiod_line_bulk bulkB = GPIOD_LINE_BULK_INITIALIZER;
	int status;
	int valA[4], valB[4];

	chipA = gpiod_chip_open(test_chip_path(0));
	chipB = gpiod_chip_open(test_chip_path(1));
	TEST_ASSERT_NOT_NULL(chipA);
	TEST_ASSERT_NOT_NULL(chipB);

	gpiod_line_bulk_init(&bulkA);

	lineA0 = gpiod_chip_get_line(chipA, 0);
	lineA1 = gpiod_chip_get_line(chipA, 1);
	lineA2 = gpiod_chip_get_line(chipA, 2);
	lineA3 = gpiod_chip_get_line(chipA, 3);
	lineB0 = gpiod_chip_get_line(chipB, 0);
	lineB1 = gpiod_chip_get_line(chipB, 1);
	lineB2 = gpiod_chip_get_line(chipB, 2);
	lineB3 = gpiod_chip_get_line(chipB, 3);

	TEST_ASSERT_NOT_NULL(lineA0);
	TEST_ASSERT_NOT_NULL(lineA1);
	TEST_ASSERT_NOT_NULL(lineA2);
	TEST_ASSERT_NOT_NULL(lineA3);
	TEST_ASSERT_NOT_NULL(lineB0);
	TEST_ASSERT_NOT_NULL(lineB1);
	TEST_ASSERT_NOT_NULL(lineB2);
	TEST_ASSERT_NOT_NULL(lineB3);

	gpiod_line_bulk_add(&bulkA, lineA0);
	gpiod_line_bulk_add(&bulkA, lineA1);
	gpiod_line_bulk_add(&bulkA, lineA2);
	gpiod_line_bulk_add(&bulkA, lineA3);
	gpiod_line_bulk_add(&bulkB, lineB0);
	gpiod_line_bulk_add(&bulkB, lineB1);
	gpiod_line_bulk_add(&bulkB, lineB2);
	gpiod_line_bulk_add(&bulkB, lineB3);

	valA[0] = 1;
	valA[1] = 0;
	valA[2] = 0;
	valA[3] = 1;
	status = gpiod_line_request_bulk_output(&bulkA, TEST_CONSUMER,
						false, valA);
	TEST_ASSERT_RET_OK(status);

	valB[0] = 0;
	valB[1] = 1;
	valB[2] = 0;
	valB[3] = 1;
	status = gpiod_line_request_bulk_output(&bulkB, TEST_CONSUMER,
						false, valB);
	TEST_ASSERT_RET_OK(status);

	memset(valA, 0, sizeof(valA));
	memset(valB, 0, sizeof(valB));

	status = gpiod_line_get_value_bulk(&bulkA, valA);
	TEST_ASSERT_RET_OK(status);
	TEST_ASSERT_EQ(valA[0], 1);
	TEST_ASSERT_EQ(valA[1], 0);
	TEST_ASSERT_EQ(valA[2], 0);
	TEST_ASSERT_EQ(valA[3], 1);

	status = gpiod_line_get_value_bulk(&bulkB, valB);
	TEST_ASSERT_RET_OK(status);
	TEST_ASSERT_EQ(valB[0], 0);
	TEST_ASSERT_EQ(valB[1], 1);
	TEST_ASSERT_EQ(valB[2], 0);
	TEST_ASSERT_EQ(valB[3], 1);

	gpiod_line_release_bulk(&bulkA);
	gpiod_line_release_bulk(&bulkB);
}
TEST_DEFINE(line_request_bulk_output,
	    "gpiod_line_request_bulk_output() - good",
	    0, { 8, 8 });

static void line_request_bulk_different_chips(void)
{
	TEST_CLEANUP(test_close_chip) struct gpiod_chip *chipA = NULL;
	TEST_CLEANUP(test_close_chip) struct gpiod_chip *chipB = NULL;
	struct gpiod_line_request_config req;
	struct gpiod_line *lineA0;
	struct gpiod_line *lineA1;
	struct gpiod_line *lineB0;
	struct gpiod_line *lineB1;
	struct gpiod_line_bulk bulk;
	int status;

	chipA = gpiod_chip_open(test_chip_path(0));
	chipB = gpiod_chip_open(test_chip_path(1));
	TEST_ASSERT_NOT_NULL(chipA);
	TEST_ASSERT_NOT_NULL(chipB);

	lineA0 = gpiod_chip_get_line(chipA, 0);
	lineA1 = gpiod_chip_get_line(chipA, 1);
	lineB0 = gpiod_chip_get_line(chipB, 0);
	lineB1 = gpiod_chip_get_line(chipB, 1);

	TEST_ASSERT_NOT_NULL(lineA0);
	TEST_ASSERT_NOT_NULL(lineA1);
	TEST_ASSERT_NOT_NULL(lineB0);
	TEST_ASSERT_NOT_NULL(lineB1);

	gpiod_line_bulk_init(&bulk);
	gpiod_line_bulk_add(&bulk, lineA0);
	gpiod_line_bulk_add(&bulk, lineA1);
	gpiod_line_bulk_add(&bulk, lineB0);
	gpiod_line_bulk_add(&bulk, lineB1);

	req.consumer = TEST_CONSUMER;
	req.direction = GPIOD_DIRECTION_INPUT;
	req.active_state = GPIOD_ACTIVE_STATE_HIGH;

	status = gpiod_line_request_bulk(&bulk, &req, NULL);
	TEST_ASSERT_NOTEQ(status, 0);
	TEST_ASSERT_EQ(gpiod_errno(), GPIOD_EBULKINCOH);
}
TEST_DEFINE(line_request_bulk_different_chips,
	    "gpiod_line_request_bulk() - different chips",
	    0, { 8, 8 });

static void line_set_value(void)
{
	TEST_CLEANUP(test_close_chip) struct gpiod_chip *chip = NULL;
	struct gpiod_line *line;
	int status;

	chip = gpiod_chip_open(test_chip_path(0));
	TEST_ASSERT_NOT_NULL(chip);

	line = gpiod_chip_get_line(chip, 2);
	TEST_ASSERT_NOT_NULL(line);

	status = gpiod_line_request_output(line, TEST_CONSUMER, false, 0);
	TEST_ASSERT_RET_OK(status);

	TEST_ASSERT_RET_OK(gpiod_line_set_value(line, 1));
	TEST_ASSERT_EQ(gpiod_line_get_value(line), 1);
	TEST_ASSERT_RET_OK(gpiod_line_set_value(line, 0));
	TEST_ASSERT_EQ(gpiod_line_get_value(line), 0);

	gpiod_line_release(line);
}
TEST_DEFINE(line_set_value,
	    "gpiod_line_set_value() - good",
	    0, { 8 });

static void line_find_by_name_good(void)
{
	TEST_CLEANUP(test_close_chip) struct gpiod_chip *chip = NULL;
	struct gpiod_line *line;

	line = gpiod_line_find_by_name("gpio-mockup-C-12");
	TEST_ASSERT_NOT_NULL(line);
	chip = gpiod_line_get_chip(line);

	TEST_ASSERT_STR_EQ(gpiod_chip_label(chip), "gpio-mockup-C");
	TEST_ASSERT_EQ(gpiod_line_offset(line), 12);
}
TEST_DEFINE(line_find_by_name_good,
	    "gpiod_line_find_by_name() - good",
	    TEST_FLAG_NAMED_LINES, { 16, 16, 32, 16 });

static void line_direction(void)
{
	TEST_CLEANUP(test_close_chip) struct gpiod_chip *chip = NULL;
	struct gpiod_line *line;
	int status;

	chip = gpiod_chip_open(test_chip_path(0));
	TEST_ASSERT_NOT_NULL(chip);

	line = gpiod_chip_get_line(chip, 5);
	TEST_ASSERT_NOT_NULL(line);

	status = gpiod_line_request_output(line, TEST_CONSUMER, false, 0);
	TEST_ASSERT_RET_OK(status);

	TEST_ASSERT_EQ(gpiod_line_direction(line), GPIOD_DIRECTION_OUTPUT);

	gpiod_line_release(line);

	status = gpiod_line_request_input(line, TEST_CONSUMER, false);
	TEST_ASSERT_RET_OK(status);

	TEST_ASSERT_EQ(gpiod_line_direction(line), GPIOD_DIRECTION_INPUT);
}
TEST_DEFINE(line_direction,
	    "gpiod_line_direction() - set & get",
	    0, { 8 });

static void line_active_state(void)
{
	TEST_CLEANUP(test_close_chip) struct gpiod_chip *chip = NULL;
	struct gpiod_line *line;
	int status;

	chip = gpiod_chip_open(test_chip_path(0));
	TEST_ASSERT_NOT_NULL(chip);

	line = gpiod_chip_get_line(chip, 5);
	TEST_ASSERT_NOT_NULL(line);

	status = gpiod_line_request_input(line, TEST_CONSUMER, false);
	TEST_ASSERT_RET_OK(status);

	TEST_ASSERT_EQ(gpiod_line_active_state(line), GPIOD_ACTIVE_STATE_HIGH);

	gpiod_line_release(line);

	status = gpiod_line_request_input(line, TEST_CONSUMER, true);
	TEST_ASSERT_RET_OK(status);

	TEST_ASSERT_EQ(gpiod_line_direction(line), GPIOD_ACTIVE_STATE_LOW);
}
TEST_DEFINE(line_active_state,
	    "gpiod_line_active_state() - set & get",
	    0, { 8 });

static void line_misc_flags(void)
{
	TEST_CLEANUP(test_close_chip) struct gpiod_chip *chip = NULL;
	struct gpiod_line_request_config config;
	struct gpiod_line *line;
	int status;

	chip = gpiod_chip_open(test_chip_path(0));
	TEST_ASSERT_NOT_NULL(chip);

	line = gpiod_chip_get_line(chip, 2);
	TEST_ASSERT_NOT_NULL(line);

	TEST_ASSERT_FALSE(gpiod_line_is_used_by_kernel(line));
	TEST_ASSERT_FALSE(gpiod_line_is_open_drain(line));
	TEST_ASSERT_FALSE(gpiod_line_is_open_source(line));

	config.direction = GPIOD_DIRECTION_INPUT;
	config.consumer = TEST_CONSUMER;
	config.active_state = GPIOD_ACTIVE_STATE_HIGH;
	config.flags = GPIOD_REQUEST_OPEN_DRAIN;

	status = gpiod_line_request(line, &config, 0);
	TEST_ASSERT_RET_OK(status);

	TEST_ASSERT(gpiod_line_is_used_by_kernel(line));
	TEST_ASSERT(gpiod_line_is_open_drain(line));
	TEST_ASSERT_FALSE(gpiod_line_is_open_source(line));

	gpiod_line_release(line);

	config.flags = GPIOD_REQUEST_OPEN_SOURCE;

	status = gpiod_line_request(line, &config, 0);
	TEST_ASSERT_RET_OK(status);

	TEST_ASSERT(gpiod_line_is_used_by_kernel(line));
	TEST_ASSERT_FALSE(gpiod_line_is_open_drain(line));
	TEST_ASSERT(gpiod_line_is_open_source(line));
}
TEST_DEFINE(line_misc_flags,
	    "gpiod_line - misc flags",
	    0, { 8 });
