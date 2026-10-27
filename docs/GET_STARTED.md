# Backend
In this project, we make use of TrafikLabs OpenAPI to fetch information about our bus stop.

# Getting started

## Acquire API key
Sign up to TrafikLab to create API keys. Then create a new key for `TrafikLab OpenAPI` and receive a free `bronze` key. This key level is limited to `100 000 requests` per month.

https://developer.trafiklab.se/register

## Acquire the TrafikLab CA Certificate
TODO decide wether to direct people to the certificate or just directly include it into the project.

## Understanding the API


https://www.trafiklab.se/api/our-apis/trafiklab-realtime-apis/openapi-specification/

https://www.trafiklab.se/sv/api/our-apis/trafiklab-realtime-apis/timetables



## Google Maps API key
For commute stops that share names, I've deemed it necessary to separate them using the Google Maps Geocoding API to resolve the `longitude` and `latitude` into States to make it easier for endusers to use the API. This only works with a *billed* Google Cloud plan.

### 1. Create a Google Account
Create a regular Google Account for *free* here: https://accounts.google.com/signin

### 2. Create Google Maps API key
To create a key go to this page and follow the instructions: https://developers.google.com/maps/documentation/geocoding/guides-v3/get-api-key

### 3. Restrict API key to Geocoding
Once the key is created, restrict it to to be only used by for Geocoding API and navigate to `Google Maps Platform -> Quotas -> Geocoding API`. Here you can set the restrictions for API requests per day.
