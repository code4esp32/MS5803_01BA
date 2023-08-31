#include "stdio.h"
#include "stdlib.h"
#include "esp_timer.h"
#include "driver/i2c.h"

#include "ms5803.h"


///////////////////////////////////////////////// ���������� ��������� ���������� ///////////////////////////////////////////////

static uint8_t ms5803_addr;
static uint16_t prom[8];				// 8 ���� ����������� ��������� PROM �������
static i2c_port_t ms5803_i2c;

static esp_timer_handle_t hTimer=0;		// ������� ��� ������� ������ ��������
static SemaphoreHandle_t hSemaphore=0;	// ������� ��� ������� ������ ��������

static uint8_t	ms5803_adc_delay[MS5803_RESOLUTION_MAX]	= {MS5803_ADC_256_DELAY_MS, MS5803_ADC_512_DELAY_MS, MS5803_ADC_1024_DELAY_MS, MS5803_ADC_2048_DELAY_MS, MS5803_ADC_4096_DELAY_MS};
static uint8_t	ms5803_adc_cmd[5] =	{0x40, 0x42, 0x44, 0x46, 0x48};

///////////////////////////////////////////////// ���������� ��������� ������� ///////////////////////////////////////////////

bool ms5803_send_cmd(uint8_t);
bool ms5803_read(uint8_t, uint8_t *, uint8_t);

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool ms5803_read(uint8_t addr, uint8_t *ptrData, uint8_t len){
// ������ len ���� �� �������� �� ������ addr � �������� �� ��������� ptrData
	i2c_cmd_handle_t link = i2c_cmd_link_create();
	i2c_master_start(link);
	i2c_master_write_byte(link, (ms5803_addr << 1), WAIT_ACK);
	i2c_master_write_byte(link, addr, WAIT_ACK); // �������� �������� ������
	// ��������� �����
	i2c_master_start(link);
	i2c_master_write_byte(link, (ms5803_addr << 1) | I2C_MASTER_READ, I2C_MASTER_ACK);
	i2c_master_read(link, ptrData, len, I2C_MASTER_LAST_NACK);
	i2c_master_stop(link);
	esp_err_t result = i2c_master_cmd_begin(ms5803_i2c, link, MS5803_I2C_TIMEOUT_TICKS);
	i2c_cmd_link_delete(link);
	return (result == ESP_OK)? 1 : 0;
}

bool ms5803_send_cmd(uint8_t cmd){
// �������� ����������� �������
	i2c_cmd_handle_t link = i2c_cmd_link_create();
	i2c_master_start(link);
	i2c_master_write_byte(link, (ms5803_addr << 1) | I2C_MASTER_WRITE, WAIT_ACK);
	i2c_master_write_byte(link, cmd, WAIT_ACK);
	i2c_master_stop(link);
	esp_err_t result = i2c_master_cmd_begin(ms5803_i2c, link, MS5803_I2C_TIMEOUT_TICKS);
	i2c_cmd_link_delete(link);
	return result == ESP_OK? 1 : 0;
}

uint8_t getCRC4(uint8_t *ptrData, uint32_t len){
// ������� �� ������� CRC4 ��� ������������������ ���� �������� �����

	uint16_t n_rem=0;
	uint8_t n_bit;

	for(uint32_t i=0; i < len; i++){
		n_rem ^= ptrData[i];

		for(n_bit = 8; n_bit > 0; n_bit--){
			if(n_rem & (0x8000)) n_rem = (n_rem << 1) ^ 0x3000;
			else n_rem = (n_rem << 1);
			}
		}
	n_rem= (0x000F & (n_rem >> 12));
	return (n_rem ^ 0x0);
}


static void ms5803_timer_cb(){
// ������� ��� ������ ��������
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	xSemaphoreGiveFromISR(hSemaphore, &xHigherPriorityTaskWoken);
}

bool ms5803_Init(i2c_port_t i2c, uint8_t addr, int16_t pin_sda, int16_t pin_scl, uint32_t speed){
// ������������� �������� I2C, ��������� ������, ����������� � �������, ���������� ��������� ���������, ������ CRC-4
	bool	i2c_installed=0, result = 1;
	i2c_config_t	config;

	// �����: �� ���� I2C ������ ������� ���� �������� � �������!
	// ���� � ������� ������� ��� ���� ��������, �� ����� ���������� 45� �������� ESP32 ����� ���������
	// ���� ������������ ������������ ������� � ����������, �� ������ ������������� �� ����������. ��������� �������������
	// ����� �������: 1/X = 1/R + 1/45000, ��� X - ������� ��������, � R - �������� � �������.

	config.mode = I2C_MODE_MASTER;
	config.scl_pullup_en = GPIO_PULLUP_ENABLE;	// ����� ��������, ��. ����
	config.sda_pullup_en = GPIO_PULLUP_ENABLE;	// ����� ��������, ��. ����
	config.sda_io_num = pin_sda;
	config.scl_io_num = pin_scl;
	config.master.clk_speed = speed;
	config.clk_flags = 0;

	for(;;){ // ������������� ���� ������ GOTO

		// ������ �������� I2C
		if((i2c_param_config(i2c, &config) != ESP_OK) ||  (i2c_driver_install(i2c, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK)) break;
		ms5803_i2c = i2c;
		ms5803_addr = addr;

		// �������� �������� ��� �������
		if(!(hSemaphore = xSemaphoreCreateBinary())) break;

		// �������� �������
		esp_timer_create_args_t timer_args = {
			.callback = &ms5803_timer_cb,
			.arg = 0,
			};
		if(esp_timer_create(&timer_args, &hTimer)!=ESP_OK) break;

		// �����
		if(!ms5803_Reset()) break;

		// TODO TODO TODO ����� ������ 10� ���������� ������� ��������.
		vTaskDelay(100);

		// �������� PROM
		for(uint8_t i = 0; i < 8; i++ ){
			prom[i] = 0;
			if(!ms5803_read(MS5803_CMD_READ_PROM + i*2, (uint8_t*)&prom[i], 2)){result = 0; break;}
			}
		if(!result) break; // ��������� ������ �� ���������� ����� ����

		// ������ CRC-4 � ������� ���������������� ����
		uint8_t crc_orig = ((uint8_t*)&prom)[15] & (0b1111);
		((uint8_t*)&prom)[15] = 0; // ����� CRC � �������
		uint8_t crc_calc = getCRC4(((uint8_t*)&prom), 16);
		if(crc_orig != crc_calc) return 0;

		// �����: ��������� ���� � ������
		for(uint8_t i = 0; i < 8; i++){
			prom[i] = FLIP_ENDIAN16(prom[i]);
			}
		return 1;
		}

	// ��� ������. ������������ ��������
	if(hTimer) esp_timer_delete(hTimer);
	if(hSemaphore) vSemaphoreDelete(hSemaphore);
	if(i2c_installed) i2c_driver_delete(ms5803_i2c);
	return 0;
}

bool ms5803_getSensors(uint8_t resolution, float *ptrTempC, float *ptrMbar){
// ������� ��������� ����� ������ � ������� � ��������� �� ����������� � ������������ � ���������

	uint8_t	data[4];
	uint32_t D1 = 0, D2 = 0;
	int32_t	dT = 0, TEMP = 0, mbarInt;
	int64_t	Offset = 0, Sensitivity = 0, T2 = 0, OFF2 = 0, Sens2 = 0;

	// �������� ������� �� ������ ��� (D1 = ��������)
	if(!ms5803_send_cmd(ms5803_adc_cmd[resolution])) return 0;
	// ������ ������� ��� ��������
	esp_timer_start_once(hTimer, 1000*ms5803_adc_delay[resolution]);
	xSemaphoreTake(hSemaphore, portMAX_DELAY);
	// �������� 24-��� ���������� ���
	if(!ms5803_read(MS5803_CMD_READ_ADC_RESULT, (uint8_t*)&data, 3)) return 0;
	D1 = ((data[0] << 16) | (data[1] << 8) | data[2]);

	// �������� ������� �� ������ ��� (D2 = �����������)
	if(!ms5803_send_cmd(ms5803_adc_cmd[resolution] | (1 << 4))) return 0;
	// ������ ������� ��� ��������
	esp_timer_start_once(hTimer, 1000*ms5803_adc_delay[resolution]);
	xSemaphoreTake(hSemaphore, portMAX_DELAY);
	// �������� 24-��� ���������� ���
	if(!ms5803_read(MS5803_CMD_READ_ADC_RESULT, (uint8_t*)&data, 3)) return 0;

	D2 = ((data[0] << 16) | (data[1] << 8) | data[2]);
	dT = D2 - (uint32_t)(((uint32_t)prom[5]) << 8);
    TEMP = (2000 + (((int64_t)dT * prom[6]) >> 23));

	// ������ ���������� ��� ����������� ����-��� � ����������� �� �����������
	if(TEMP < 2000){ // ����� 20�
		T2 = (int32_t) (((int64_t)dT * dT) >> 31);
		OFF2 = 3 * ((TEMP-2000) * (TEMP-2000));
		Sens2 = 7 * (((TEMP-2000) * (TEMP-2000)) >> 3);

		if(TEMP < -1500){ // ���� -15�
			Sens2 += (((TEMP+1500) * (TEMP+1500)) << 1);
			}
    	}
    else{ // ���� 20�
		T2 = 0;
		OFF2 = 0;
		Sens2 = 0;
		if(TEMP > 4500){ // ���� 45�
			Sens2 -= (((TEMP-4500) * (TEMP-4500)) >> 3);
			}
    	}

	// ������ �������� ����������� ����-���
	Offset = ((uint64_t)prom[2] << 16) + (((int64_t)prom[4] * dT) >> 7) - OFF2;
	Sensitivity = ((int64_t)prom[1] << 15) + ((((int64_t)prom[3] * dT)) >> 8) - Sens2;

	// ��������� �����������
	TEMP -= T2;

	// ������ ������������������ ��������
	mbarInt = (((((int64_t)D1 * Sensitivity) >> 21) - Offset) >> 15);
	// �������� �����������
	*ptrMbar = (float)mbarInt / 100;
	*ptrTempC  = (float)TEMP / 100;
	return 1;
}


bool ms5803_Reset(void){
	return ms5803_send_cmd(MS5803_CMD_RESET);
}
