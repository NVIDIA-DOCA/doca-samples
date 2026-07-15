# DOCA AES-GCM Encrypt and Decrypt Samples

These samples illustrate how to encrypt and decrypt data using AES-GCM with the DOCA library.

## AES-GCM Encrypt Sample

This sample demonstrates how to encrypt data with AES-GCM.

### Sample Logic:
1. Locating a DOCA device.
2. Initializing the required DOCA Core structures.
3. Setting the AES-GCM encrypt task configuration.
4. Populating the DOCA memory map with two relevant buffers.
5. Allocating elements in the DOCA buffer inventory for each buffer.
6. Creating a DOCA AES-GCM key.
7. Allocating and initializing the AES-GCM encrypt task.
8. Submitting the AES-GCM encrypt task.
9. Retrieving the AES-GCM encrypt task once it is done.
10. Checking the task result.
11. Destroying all AES-GCM and DOCA Core structures.

### References:
- `aes_gcm_encrypt/aes_gcm_encrypt_sample.c`
- `aes_gcm_encrypt/aes_gcm_encrypt_main.c`
- `aes_gcm_encrypt/meson.build`

---

## AES-GCM Decrypt Sample

This sample demonstrates how to decrypt data with AES-GCM.

### Sample Logic:
1. Locating a DOCA device.
2. Initializing the required DOCA Core structures.
3. Setting the AES-GCM decrypt task configuration.
4. Populating the DOCA memory map with two relevant buffers.
5. Allocating elements in the DOCA buffer inventory for each buffer.
6. Creating a DOCA AES-GCM key.
7. Allocating and initializing the AES-GCM decrypt task.
8. Submitting the AES-GCM decrypt task.
9. Retrieving the AES-GCM decrypt task once it is done.
10. Checking the task result.
11. Destroying all AES-GCM and DOCA Core structures.

### References:
- `aes_gcm_decrypt/aes_gcm_decrypt_sample.c`
- `aes_gcm_decrypt/aes_gcm_decrypt_main.c`
- `aes_gcm_decrypt/meson.build`
