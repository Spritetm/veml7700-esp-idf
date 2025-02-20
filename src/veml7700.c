/**
 * @file veml7700.c
 * 
 * @author Kristijan Grozdanovski (kgrozdanovski7@gmail.com)
 * 
 * @brief Vishay VEML7700 Light Sensor driver for integration with ESP-IDF framework.
 * 
 * @version 1
 * 
 * @date 2021-12-11
 * 
 * @copyright Copyright (c) 2021, Kristijan Grozdanovski
 * All rights reserved.
 * 
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. 
 */
#include "veml7700.h"

/**
 * @brief List of all possible values for configuring sensor gain.
 * 
 */
static const uint8_t gain_values[VEML7700_GAIN_OPTIONS_COUNT] = {
    VEML7700_GAIN_2,
    VEML7700_GAIN_1,
    VEML7700_GAIN_1_8,
    VEML7700_GAIN_1_4
};
/**
 * @brief List of all possible values for configuring sensor integration time.
 * 
 */
static const uint8_t integration_time_values[VEML7700_IT_OPTIONS_COUNT] = {
    VEML7700_IT_800MS,
    VEML7700_IT_400MS,
    VEML7700_IT_200MS,
    VEML7700_IT_100MS,
    VEML7700_IT_50MS,
    VEML7700_IT_25MS
};

/**
 * @brief Proper resolution multipliers mapped to gain-integration time combination.
 * 
 * @note Source: Official Vishay VEML7700 Application Note, rev. 20-Sep-2019
 * 
 * @link https://www.vishay.com/docs/84323/designingveml7700.pdf
 */
static const float resolution_map[VEML7700_IT_OPTIONS_COUNT][VEML7700_GAIN_OPTIONS_COUNT] = {
    {0.0036, 0.0072, 0.0288, 0.0576},
    {0.0072, 0.0144, 0.0576, 0.1152},
    {0.0144, 0.0288, 0.1152, 0.2304},
    {0.0288, 0.0576, 0.2304, 0.4608},
    {0.0576, 0.1152, 0.4608, 0.9216},
    {9.1152, 0.2304, 0.9216, 1.8432}
};
/**
 * @brief Maximum luminocity mapped to gain-integration time combination.
 * 
 * @note Source: Official Vishay VEML7700 Application Note, rev. 20-Sep-2019
 * 
 * @link https://www.vishay.com/docs/84323/designingveml7700.pdf
 */
static const uint32_t maximums_map[VEML7700_IT_OPTIONS_COUNT][VEML7700_GAIN_OPTIONS_COUNT] = {
    {236, 472, 1887, 3775},
    {472, 944, 3775, 7550},
    {944, 1887, 7550, 15099},
    {1887, 3775, 15099, 30199},
    {3775, 7550, 30199, 60398},
    {7550, 15099, 60398, 120796}
};

/**
 * @brief Component tag to be used for ESP LOG messages.
 * 
 */
static const char* VEML7700_TAG = "VEML7700";

/**
 * @brief The default I2C address of the sensor.
 * 
 * @todo Support configuration through sdkconfig.
 * 
 */
const uint8_t veml7700_device_address = VEML7700_I2C_ADDR;

/**
 * @brief The currently active sensor configuration.
 * 
 */
static struct veml7700_config veml7700_configuration;

static struct veml7700_config veml7700_get_default_config()
{
	struct veml7700_config configuration;

	configuration.gain = VEML7700_GAIN_1_8;
	configuration.integration_time = VEML7700_IT_100MS;	// Start with 100ms to avoid any saturation effect
	configuration.persistance = VEML7700_PERS_1;
	configuration.interrupt_enable = false;
	configuration.shutdown = VEML7700_POWERSAVE_MODE1;

	return configuration;
}

static esp_err_t veml7700_optimize_configuration(uint32_t *lux)
{
	// Make sure this isn't the smallest maximum
	if (veml7700_configuration.maximum_lux == veml7700_get_lowest_maximum_lux()) {
		ESP_LOGD(VEML7700_TAG, "Already configured with maximum resolution.");
		return ESP_FAIL;
	}
	if (veml7700_configuration.maximum_lux == veml7700_get_maximum_lux()) {
		ESP_LOGD(VEML7700_TAG, "Already configured for maximum luminocity.");
		return ESP_FAIL;
	}

	if (ceil(*lux) >= veml7700_get_current_maximum_lux()) {
		// Decrease resolution
		decrease_resolution();
	} else if (*lux < veml7700_get_lower_maximum_lux(lux)) {
		// Increase resolution
		increase_resolution();
	} else {
		ESP_LOGD(VEML7700_TAG, "Configuration already optimal.");
		return ESP_FAIL;
	}

	ESP_LOGD(VEML7700_TAG, "Configuration optimized.");
	return ESP_OK;
}

static uint32_t veml7700_get_current_maximum_lux()
{
	int gain_index = veml7700_get_gain_index(veml7700_configuration.gain);
	int it_index = veml7700_get_it_index(veml7700_configuration.integration_time);

	return maximums_map[it_index][gain_index];
}

static uint32_t veml7700_get_lower_maximum_lux(uint32_t* lux)
{
	int gain_index = veml7700_get_gain_index(veml7700_configuration.gain);
	int it_index = veml7700_get_it_index(veml7700_configuration.integration_time);

	// Find the next smallest 'maximum' value in the mapped maximum luminocities
	if ((gain_index > 0) && (it_index > 0)) {
		if (maximums_map[it_index][gain_index - 1] >= maximums_map[it_index - 1][gain_index]) {
			return maximums_map[it_index][gain_index - 1];
		} else {
			return maximums_map[it_index - 1][gain_index];
		}
	} else if ((gain_index > 0) && (it_index == 0)) {
		return maximums_map[it_index][gain_index - 1];
	} else {
		return maximums_map[it_index - 1][gain_index];
	}
}

static uint32_t veml7700_get_lowest_maximum_lux()
{
	return maximums_map[0][0];
}

static uint32_t veml7700_get_maximum_lux()
{
	return maximums_map[VEML7700_IT_OPTIONS_COUNT - 1][VEML7700_GAIN_OPTIONS_COUNT - 1];
}

static int veml7700_get_gain_index(uint8_t gain)
{
	return indexOf(gain, &gain_values, VEML7700_GAIN_OPTIONS_COUNT);
}

static int veml7700_get_it_index(uint8_t integration_time)
{
	return indexOf(integration_time, &integration_time_values, VEML7700_IT_OPTIONS_COUNT);
}

static uint8_t indexOf(uint8_t elm, uint8_t *ar, uint8_t len)
{
    while (len--) { if (ar[len] == elm) { return len; } } return -1;
}

static void decrease_resolution()
{
	// Identify the indexes of the currently configured values
	int gain_index = veml7700_get_gain_index(veml7700_configuration.gain);
	int it_index = veml7700_get_it_index(veml7700_configuration.integration_time);

	ESP_LOGD(VEML7700_TAG, "Decreasing sensor resolution...\n");

	// If this is the last gain or integration time setting, increment the other property
	if (gain_index == 3) {
		veml7700_configuration.integration_time = integration_time_values[it_index + 1];
	} else if (it_index == 5) {
		veml7700_configuration.gain = gain_values[gain_index + 1];
	} else {
		// Check which adjacent value is bigger than the current, but choose the smaller if both are
		if (resolution_map[it_index + 1][gain_index] > veml7700_configuration.resolution) {
			if (resolution_map[it_index + 1][gain_index] <= resolution_map[it_index][gain_index + 1]) {
				veml7700_configuration.integration_time = integration_time_values[it_index + 1];
			}
		} else if (resolution_map[it_index][gain_index + 1] > veml7700_configuration.resolution) {
			if (resolution_map[it_index][gain_index + 1] <= resolution_map[it_index + 1][gain_index]) {
				veml7700_configuration.gain = gain_values[gain_index + 1];
			}
		} else {
			ESP_LOGE(VEML7700_TAG, "Failed to decrease sensor resolution.");
		}
	}

	// Update the sensor configuration
	veml7700_set_config(&veml7700_configuration);
}

static void increase_resolution()
{
	int gain_index = veml7700_get_gain_index(veml7700_configuration.gain);
	int it_index = veml7700_get_it_index(veml7700_configuration.integration_time);

	if ((gain_index > 0) && (it_index > 0)) {
		if (maximums_map[it_index][gain_index - 1] >= maximums_map[it_index - 1][gain_index]) {
			veml7700_configuration.gain = gain_values[gain_index - 1];
		} else {
			veml7700_configuration.integration_time = integration_time_values[it_index - 1];
		}
	} else if ((gain_index > 0) && (it_index == 0)) {
		veml7700_configuration.gain = gain_values[gain_index - 1];
	} else {
		veml7700_configuration.integration_time = integration_time_values[it_index - 1];
	}

	// Update the sensor configuration
	veml7700_set_config(&veml7700_configuration);
}

static esp_err_t veml7700_i2c_read(uint8_t *dev_addr, uint8_t reg_addr, uint16_t *reg_data, uint8_t len)
{
	esp_err_t espRc;

	i2c_cmd_handle_t cmd = i2c_cmd_link_create();

	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (*dev_addr << 1) | I2C_MASTER_WRITE, true);
	i2c_master_write_byte(cmd, reg_addr, true);

	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (*dev_addr << 1) | I2C_MASTER_READ, true);

	i2c_master_read(cmd, reg_data, len, I2C_MASTER_LAST_NACK);
	i2c_master_stop(cmd);

	*reg_data = (*reg_data >> 8) | (*reg_data << 8);  // Swap if LSB first

	espRc = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 2000 / portTICK_PERIOD_MS);

	i2c_cmd_link_delete(cmd);

	return espRc;
}

static esp_err_t veml7700_i2c_write(uint8_t *dev_addr, uint8_t reg_addr, uint16_t *reg_data, uint8_t len)
{
	esp_err_t espRc;
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();

	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (*dev_addr << 1) | I2C_MASTER_WRITE, false);
	i2c_master_write_byte(cmd, reg_addr, false);

	i2c_master_write(cmd, reg_data, len, false);
	
	i2c_master_stop(cmd);

	espRc = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);

	i2c_cmd_link_delete(cmd);

	return espRc;
}

esp_err_t veml7700_initialize()
{
	// Define the sensor configuration globally
	veml7700_configuration = veml7700_get_default_config();

	return veml7700_set_config(&veml7700_configuration);
}

esp_err_t veml7700_set_config(struct veml7700_config *configuration)
{
	uint16_t config_data = ( 
		(configuration->gain << 11) |
		(configuration->integration_time << 6) |
		(configuration->persistance << 4) |
		(configuration->interrupt_enable << 1) |
		(configuration->shutdown << 0)
	);

	// Set the resolution on the global configuration struct
	veml7700_configuration.resolution = veml7700_get_resolution();
	// Set the current maximum value on the global configuration struct
	veml7700_configuration.maximum_lux = veml7700_get_current_maximum_lux();

	return veml7700_i2c_write(
		&veml7700_device_address, 
		VEML7700_ALS_CONFIG, 
		&config_data,
		2
	);
}

esp_err_t veml7700_read_als_lux(double* lux)
{
	esp_err_t i2c_result;
	uint16_t reg_data;
	
	i2c_result = veml7700_i2c_read(&veml7700_device_address, VEML7700_ALS_DATA, &reg_data, 2);
	if (i2c_result != 0) {
		ESP_LOGW(VEML7700_TAG, "veml7700_i2c_read() returned %d", i2c_result);
		return i2c_result;;
	}

	*lux = reg_data * veml7700_configuration.resolution;

	return ESP_OK;
}

esp_err_t veml7700_read_als_lux_auto(double* lux)
{
	veml7700_read_als_lux(lux);

	ESP_LOGD(VEML7700_TAG, "Configured maximum luminocity: %d\n", veml7700_configuration.maximum_lux);
	ESP_LOGD(VEML7700_TAG, "Configured resolution: %0.4f\n", veml7700_configuration.resolution);
	
	// Calculate and automatically reconfigure the optimal sensor configuration
	esp_err_t optimize = veml7700_optimize_configuration(lux);
	if (optimize == ESP_OK) {
		// Read again
		return veml7700_read_als_lux(lux);
	}

	return ESP_OK;
}

esp_err_t veml7700_read_white_lux(double* lux)
{
	esp_err_t i2c_result;
	uint16_t reg_data;
	
	i2c_result = veml7700_i2c_read(&veml7700_device_address, VEML7700_WHITE_DATA, &reg_data, 2);
	if (i2c_result != 0) {
		ESP_LOGW(VEML7700_TAG, "veml7700_i2c_read() returned %d", i2c_result);
		return i2c_result;
	}

	*lux = reg_data * veml7700_configuration.resolution;

	return ESP_OK;
}

esp_err_t veml7700_read_white_lux_auto(double* lux)
{
	veml7700_read_white_lux(lux);

	ESP_LOGD(VEML7700_TAG, "Configured maximum luminocity: %d\n", veml7700_configuration.maximum_lux);
	ESP_LOGD(VEML7700_TAG, "Configured resolution: %0.4f\n", veml7700_configuration.resolution);
	
	// Calculate and automatically reconfigure the optimal sensor configuration
	esp_err_t optimize = veml7700_optimize_configuration(lux);
	if (optimize == ESP_OK) {
		// Read again
		return veml7700_read_white_lux(lux);
	}

	return ESP_OK;
}

float veml7700_get_resolution()
{
	int gain_index = veml7700_get_gain_index(veml7700_configuration.gain);
	int it_index = veml7700_get_it_index(veml7700_configuration.integration_time);

	return resolution_map[it_index][gain_index];
}